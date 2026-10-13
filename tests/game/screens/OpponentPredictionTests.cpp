#include <algorithm>
#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "OpponentPredictionFixture.h"
#include "TestSupport.h"
#include "game/config/GameConfig.h"
#include "game/screens/LevelOpponents.h"
#include "game/screens/OpponentPlacements.h"
#include "game/screens/OpponentPrediction.h"
#include "game/screens/OpponentResources.h"
#include "game/world/BodyCollision.h"
#include "game/world/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void sameBox(const Obstacle& first, const Obstacle& second) {
    CHECK(first.centre == second.centre);
    CHECK(first.yaw == second.yaw);
    CHECK(first.halfAcross == second.halfAcross);
    CHECK(first.halfAlong == second.halfAlong);
    CHECK(first.height == second.height);
    CHECK(first.solid == second.solid);
    CHECK(first.cylinderRadius == second.cylinderRadius);
    REQUIRE(first.enemyItem.has_value() == second.enemyItem.has_value());
    if (first.enemyItem) {
        CHECK(first.enemyItem->radius == second.enemyItem->radius);
        CHECK(first.enemyItem->cylindrical == second.enemyItem->cylindrical);
    }
}

using test::ContinuationFixture;

TEST_CASE("private opponent owner restores and continues independent native clocks and bodies",
          "[rollback][opponent-prediction][assets]") {
    const auto gain = GENERATE(0.667f, 1.0f, 1.5f);
    const auto initialPlayers = GENERATE(1, 4);
    const auto ticks = GENERATE(1, 2);
    CAPTURE(gain, initialPlayers, ticks);
    ContinuationFixture fixture(gain, initialPlayers);
    const auto slot = fixture.spawn();
    for (s32 tick = 0; tick < Enemies::kPlacedStun; ++tick) {
        fixture.native.advance(fixture.frame());
    }
    const auto authority = fixture.capture();
    REQUIRE(fixture.prediction.begin(7, 100, authority, fixture.collision));
    REQUIRE(fixture.prediction.enemies() != nullptr);
    CHECK(fixture.prediction.enemies() != &fixture.enemies);
    const auto created = fixture.device.texturesCreated;
    const auto updated = fixture.device.textureUpdates;
    const auto frozen = fixture.enemies.targets();
    std::vector<OpponentState> expected;
    for (u64 tick = 101; tick <= 120; ++tick) {
        fixture.native.advance(fixture.frame(ticks));
        expected.push_back(fixture.capture());
    }
    const auto nativeFinal = fixture.capture();
    for (u64 tick = 101; tick <= 120; ++tick) {
        CAPTURE(tick);
        REQUIRE(fixture.prediction.advance(tick, fixture.frame(ticks)) ==
                OpponentSimulation::Result::Advanced);
        CHECK(fixture.prediction.checkpoint() == expected[static_cast<usize>(tick - 101)]);
        CHECK(fixture.capture() == nativeFinal); // private replay never touches the host owner
    }
    const auto* continued = fixture.prediction.enemies();
    REQUIRE(continued != nullptr);
    CHECK(continued->positionOf(slot) !=
          authority.enemies.enemies[static_cast<usize>(slot)].position);
    const auto live = continued->targets();
    REQUIRE(live.size() == 1);
    // Real player collision now observes the moving native enemy, not a frozen
    // render snapshot. The same candidate step must produce a different result.
    const Vec3 destination = live.front().base;
    const Vec3 from = destination + Vec3{4, 0, 0};
    CHECK(BodyCollision::resolve(from, destination, 1, 6, live) !=
          BodyCollision::resolve(from, destination, 1, 6, frozen));
    const auto shown = fixture.prediction.knownEnemies();
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].slot == static_cast<u32>(slot));
    CHECK(shown[0].state == nativeFinal.enemies.enemies[static_cast<usize>(slot)]);
    // A second correction pass starts from the same native boundary, not the
    // end of the first pass, and performs no GPU/resource work.
    REQUIRE(fixture.prediction.begin(7, 100, authority, fixture.collision));
    for (u64 tick = 101; tick <= 120; ++tick) {
        REQUIRE(fixture.prediction.advance(tick, fixture.frame(ticks)) ==
                OpponentSimulation::Result::Advanced);
    }
    CHECK(fixture.prediction.checkpoint() == nativeFinal);
    CHECK(fixture.device.texturesCreated == created);
    CHECK(fixture.device.textureUpdates == updated);
}

TEST_CASE("private opponent owner checks trusted tuning and topology before any restore",
          "[rollback][opponent-prediction][assets]") {
    ContinuationFixture fixture;
    fixture.spawn();
    const auto authority = fixture.capture();
    REQUIRE(fixture.prediction.begin(7, 100, authority, fixture.collision));
    REQUIRE(fixture.prediction.advance(101, fixture.frame()) ==
            OpponentSimulation::Result::Advanced);
    const auto before = fixture.prediction.checkpoint();
    auto invalid = authority;
    SECTION("enemy difficulty") {
        invalid.enemies.scales.speed += 0.1f;
    }
    SECTION("generator difficulty") {
        invalid.generators.scales.rate += 0.1f;
    }
    SECTION("initial party size") {
        invalid.enemies.scales.players = 4;
    }
    SECTION("native pool size") {
        invalid.enemies.most = authority.enemies.most == 1 ? Enemies::kMost : 1;
    }
    SECTION("contact tie grid") {
        invalid.enemies.contactGridOrigin.x += 1;
    }
    SECTION("patrol route") {
        if (invalid.enemies.lookouts.empty()) {
            invalid.enemies.lookouts.points.push_back({0, 0, 0});
            invalid.enemies.lookouts.next.push_back(0);
        } else {
            invalid.enemies.lookouts.points.front().x += 1;
        }
    }
    SECTION("malformed missile") {
        invalid.missiles.shrink = std::numeric_limits<f32>::quiet_NaN();
    }
    SECTION("missing trusted stock") {
        invalid.enemies.stocks.clear();
    }
    CHECK_FALSE(fixture.prediction.begin(7, 102, invalid, fixture.collision));
    CHECK(fixture.prediction.checkpoint() == before);
    CHECK_FALSE(fixture.prediction.active());
    CHECK(fixture.prediction.enemies() == nullptr);
    CHECK(fixture.prediction.knownEnemies().empty());
}

TEST_CASE("private opponent owner rejects whole passes and restores all earlier successful ticks",
          "[rollback][opponent-prediction][assets]") {
    ContinuationFixture fixture;
    fixture.spawn();
    const auto authority = fixture.capture();
    REQUIRE(fixture.prediction.begin(7, 100, authority, fixture.collision));
    for (u64 tick = 101; tick <= 106; ++tick) {
        REQUIRE(fixture.prediction.advance(tick, fixture.frame()) ==
                OpponentSimulation::Result::Advanced);
    }
    REQUIRE(fixture.prediction.checkpoint() != authority);
    auto frame = fixture.frame();
    SECTION("unsupported gameplay interactions") {
        fixture.players[0].reflects = true;
        CHECK(fixture.prediction.advance(107, frame) == OpponentSimulation::Result::Unsupported);
    }
    SECTION("invalid work scale") {
        frame.missileSpeed = std::numeric_limits<f32>::max();
        CHECK(fixture.prediction.advance(107, frame) == OpponentSimulation::Result::Invalid);
    }
    SECTION("out of order world step") {
        CHECK(fixture.prediction.advance(106, frame) == OpponentSimulation::Result::Invalid);
    }
    SECTION("caller abandons the player replay pass") {
        fixture.prediction.resetReplay();
    }
    CHECK(fixture.prediction.checkpoint() == authority);
    CHECK(fixture.prediction.tick() == 100);
    CHECK_FALSE(fixture.prediction.active());
    CHECK(fixture.prediction.generators() == nullptr);
    CHECK(fixture.prediction.missiles() == nullptr);
    CHECK(fixture.prediction.collision() == nullptr);
    CHECK(fixture.prediction.advance(101, fixture.frame()) == OpponentSimulation::Result::Invalid);
    REQUIRE(fixture.prediction.begin(7, 100, authority, fixture.collision));
    CHECK(fixture.prediction.active());
}

TEST_CASE("private opponent owner bounds held phases and invalidates stale replay output",
          "[rollback][opponent-prediction][assets]") {
    ContinuationFixture fixture;
    fixture.spawn();
    const auto authority = fixture.capture();
    REQUIRE(fixture.prediction.begin(7, 100, authority, fixture.collision));
    for (u64 tick = 101; tick <= 100 + OpponentPrediction::kMaxTicks; ++tick) {
        REQUIRE(fixture.prediction.advance(tick, fixture.frame(), true) ==
                OpponentSimulation::Result::Advanced);
        CHECK(fixture.prediction.checkpoint() == authority);
    }
    CHECK(fixture.prediction.advance(221, fixture.frame(), true) ==
          OpponentSimulation::Result::Invalid);
    CHECK_FALSE(fixture.prediction.active());
    CHECK(fixture.prediction.checkpoint() == authority);
    CHECK_FALSE(fixture.prediction.begin(6, 101, authority, fixture.collision));
    CHECK_FALSE(fixture.prediction.begin(7, 99, authority, fixture.collision));
    REQUIRE(fixture.prediction.begin(8, 0, authority, fixture.collision));
    CHECK(fixture.prediction.epoch() == 8);
    CHECK(fixture.prediction.tick() == 0);
    CHECK_FALSE(fixture.prediction.open(fixture.device, fixture.root, fixture.world,
                                        fixture.enemies, 1, 2.0f));
    CHECK(fixture.prediction.active());
    CHECK(fixture.prediction.checkpoint() == authority);
    fixture.prediction.clear();
    CHECK_FALSE(fixture.prediction.loaded());
    CHECK_FALSE(fixture.prediction.active());
    CHECK_FALSE(fixture.prediction.checkpoint());
}

TEST_CASE("private generator contacts keep native slot and obstacle ordering",
          "[rollback][opponent-prediction][assets]") {
    ContinuationFixture fixture(1, 1, "G1");
    const auto authority = fixture.capture();
    REQUIRE(fixture.prediction.begin(7, 100, authority, fixture.collision));
    const auto* generators = fixture.prediction.generators();
    REQUIRE(generators != nullptr);
    const auto contacts = generators->identifiedObstacles();
    const auto boxes = generators->obstacles();
    REQUIRE_FALSE(contacts.empty());
    REQUIRE(contacts.size() == boxes.size());
    for (usize index = 0; index < boxes.size(); ++index) {
        CAPTURE(index);
        sameBox(contacts[index].box, boxes[index]);
        sameBox(contacts[index].box, generators->boxOf(static_cast<s32>(contacts[index].slot)));
    }
}

TEST_CASE("private generator births affect contacts without publishing speculative lifetimes",
          "[rollback][opponent-prediction][assets]") {
    ContinuationFixture fixture(1, 1, "G1");
    auto authority = fixture.capture();
    REQUIRE(authority.enemies.nextInstance == 1);
    const auto chosen = std::ranges::find_if(authority.generators.generators, [](const auto& slot) {
        return slot.kind >= 0 && slot.kind < kSwarmKindCount && slot.state > 0 && slot.most > 0;
    });
    REQUIRE(chosen != authority.generators.generators.end());
    const auto generator = static_cast<usize>(chosen - authority.generators.generators.begin());
    for (usize index = 0; index < authority.generators.generators.size(); ++index) {
        auto& slot = authority.generators.generators[index];
        if (index != generator) {
            slot.state = 0;
            slot.health = 0;
            slot.box.solid = false;
        }
    }
    chosen->algorithm = kChaseWay;
    chosen->countdown = 0;
    chosen->presence = Generators::Slot::Presence::Shown;
    chosen->box.solid = true;
    authority.generators.view.reset();
    authority.generators.attention.reset();
    fixture.players[0].position = chosen->position + chosen->direction * 25.0f;
    REQUIRE(fixture.native.restore(authority, fixture.resources));
    REQUIRE(fixture.prediction.begin(7, 100, authority, fixture.collision));
    fixture.native.advance(fixture.frame());
    const auto expected = fixture.capture();
    REQUIRE(expected.enemies.nextInstance > authority.enemies.nextInstance);
    REQUIRE(fixture.prediction.advance(101, fixture.frame()) ==
            OpponentSimulation::Result::Advanced);
    CHECK(fixture.prediction.checkpoint() == expected);
    REQUIRE(fixture.prediction.enemies() != nullptr);
    CHECK_FALSE(fixture.prediction.enemies()->targets().empty());
    CHECK(fixture.prediction.knownEnemies().empty());
    // Once the real host confirms those IDs at a later boundary they become
    // eligible for local presentation, without creating any extra native body.
    REQUIRE(fixture.prediction.begin(7, 101, expected, fixture.collision));
    CHECK_FALSE(fixture.prediction.knownEnemies().empty());
}

TEST_CASE(
    "private owner binds native G1 placement records without accepting packet-authored births",
    "[rollback][opponent-prediction][opponent-placements][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents native;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, {}, 0);
    LevelOpponents::Resources resources{device, world, weapons, effects, audio, root, 1};
    resources.standOnSight = true;
    native.open(resources, players);
    auto authority =
        OpponentResources::capture(native.generators(), native.enemies(), native.missiles());
    REQUIRE(authority);
    authority->placements = native.placements().checkpoint();
    REQUIRE(authority->placements);
    REQUIRE_FALSE(authority->placements->roster.empty());
    REQUIRE_FALSE(authority->placements->pending.empty());
    REQUIRE(native.statues().count() > 0);
    for (usize index = 0; index < native.statues().count(); ++index) {
        const auto& statue = native.statues().placement(index);
        const s32 kind = statue.enemy                          ? statue.enemy->kind
                         : statue.kind == CombatantKind::Golem ? kGolemEnemyKind
                                                               : kGargoyleEnemyKind;
        CHECK(std::ranges::none_of(authority->placements->roster, [&](const auto& row) {
            return row.kind == kind && row.spawn.position == statue.instance.position;
        }));
    }

    // Bind independently from immutable level metadata, just as a new guest
    // does. The complete native roster must match, not a compacted pending list.
    REQUIRE(std::ranges::any_of(authority->placements->roster, [](const auto& row) {
        return row.kind >= kSwarmKindCount || row.carried.has_value();
    }));
    std::vector<std::pair<bool, Vec3>> pickups;
    for (usize index = 0; index < world.placedItems().size(); ++index) {
        const auto& item = world.placedItems().item(index);
        pickups.emplace_back(item.carried, item.position);
    }
    OpponentPrediction prediction;
    REQUIRE(prediction.open(device, root, world, native.enemies(), 1, 1));
    for (usize index = 0; index < pickups.size(); ++index) {
        CHECK(world.placedItems().item(index).carried == pickups[index].first);
        CHECK(world.placedItems().item(index).position == pickups[index].second);
    }
    const auto uploads = device.texturesCreated;
    const auto updates = device.textureUpdates;
    REQUIRE(prediction.begin(7, 100, *authority, world.collision()));
    CHECK(prediction.checkpoint() == authority);
    auto forged = *authority;
    forged.placements->roster.front().spawn.position.x += 1;
    REQUIRE(forged.valid());
    CHECK_FALSE(prediction.begin(7, 101, forged, world.collision()));
    CHECK(prediction.checkpoint() == authority);
    // Restoration is not permission to activate a General/held pickup. With no
    // watch frustum native loading selects every pending row, so the private
    // phase must refuse before consuming any placement or mutating any pool.
    REQUIRE(prediction.begin(7, 101, *authority, world.collision()));
    CHECK(prediction.advance(102, {}) == OpponentSimulation::Result::Unsupported);
    CHECK_FALSE(prediction.active());
    CHECK(prediction.checkpoint() == authority);
    CHECK(device.texturesCreated == uploads);
    CHECK(device.textureUpdates == updates);
}

} // namespace
