#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/LevelOpponents.h"
#include "game/enemies/PlacedEnemies.h"
#include "game/enemies/PlacedEnemiesCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

PlacedEnemies::Placement placement(const Vec3& position, s32 kind = kGruntKind) {
    PlacedEnemies::Placement value;
    value.kind = kind;
    value.spawn.kind = kind;
    value.spawn.position = position;
    value.spawn.placed = true;
    value.spawn.priority = EnemySpawn::Priority::Visible;
    value.viewRadius = 1;
    return value;
}

// The original LevelOpponents phase, independent of the extracted owner.
template <class Stand>
void originalPhase(std::vector<PlacedEnemies::Placement>& pending,
                   const std::optional<ViewVolume>& view, const Vec3& attention, Stand stand) {
    std::erase_if(pending, [&](const auto& record) {
        if (view && (!view->sees(record.spawn.position, record.viewRadius) ||
                     glm::distance(attention, record.spawn.position) > 50.0f)) {
            return false;
        }
        auto standing = record;
        if (!view) {
            standing.spawn.priority = EnemySpawn::Priority::FreeSlotOnly;
        }
        stand(standing);
        return true;
    });
}

std::vector<PlacedEnemies::Placement> pendingRecords(const PlacedEnemies& owner) {
    std::vector<PlacedEnemies::Placement> result;
    for (const usize index : owner.pending()) {
        result.push_back(owner.roster()[index]);
    }
    return result;
}

TEST_CASE("placed enemy checkpoint retains complete metadata and ordered native activation",
          "[rollback][placed-enemies]") {
    auto varied = placement({0, 0, 10});
    varied.spawn.zigZagSide = -1;
    varied.spawn.tier = 4;
    varied.spawn.algorithm = kThrowWay;
    varied.spawn.direction = {0.25f, 0, 0.75f};
    varied.spawn.clearance = 2;
    varied.spawn.generator = 7;
    varied.spawn.patrolBirth = true;
    varied.spawn.allBirthDirections = true;
    varied.spawn.asleep = true;
    varied.spawn.throwInterval = 0.75f;
    varied.spawn.sight = 19.5f;
    varied.facing = 0.625f;
    varied.viewRadius = 2.5f;
    varied.sight = -3;
    varied.carried = 37;
    const std::vector roster{placement({100, 0, 10}), varied, placement({0, 0, 51}), varied,
                             placement({0, 0, 50})};
    PlacedEnemies owner;
    owner.bind(roster);
    const ViewVolume view;
    owner.observe(view, {});
    const auto checkpoint = PlacedEnemiesCheckpoint::capture(owner);
    REQUIRE(checkpoint);
    CHECK(checkpoint->roster == roster);
    CHECK(checkpoint->pending == std::vector<usize>{0, 1, 2, 3, 4});
    CHECK(owner.inView({0, 0, 50}, 1));
    CHECK_FALSE(owner.inView({0, 0, 50.01f}, 1));
    CHECK_FALSE(owner.inView({100, 0, 10}, 1));
    const auto expected = owner.activations();
    REQUIRE(checkpoint->activations());
    CHECK(*checkpoint->activations() == expected);
    CHECK(PlacedEnemiesCheckpoint::capture(owner) == checkpoint);
    REQUIRE(expected.size() == 3);
    CHECK(expected[0] == varied);
    CHECK(expected[1] == varied); // Identical records are distinct original placements.
    auto native = roster;
    std::vector<PlacedEnemies::Placement> oracle;
    originalPhase(native, view, {}, [&](const auto& standing) { oracle.push_back(standing); });
    std::vector<PlacedEnemies::Placement> actual;
    owner.activate([&](const auto& standing) { actual.push_back(standing); });
    CHECK(actual == oracle);
    CHECK(actual == expected);
    CHECK(pendingRecords(owner) == native);
    CHECK(owner.pending().size() == 2);
    const auto after = PlacedEnemiesCheckpoint::capture(owner);
    REQUIRE(after);
    CHECK(after->pending == std::vector<usize>{0, 2});
    REQUIRE(PlacedEnemiesCheckpoint::restore(owner, *checkpoint));
    actual.clear();
    owner.activate([&](const auto& standing) { actual.push_back(standing); });
    CHECK(actual == expected);
    CHECK(PlacedEnemiesCheckpoint::capture(owner) == after);
}

TEST_CASE("placed enemy no-view activation consumes failed attempts in native order",
          "[rollback][placed-enemies]") {
    const std::vector roster{placement({0, 0, -500}), placement({100, 0, 10}),
                             placement({0, 0, 500})};
    PlacedEnemies owner;
    owner.bind(roster);
    owner.observe(std::nullopt, {123, 456, 789});
    const auto before = PlacedEnemiesCheckpoint::capture(owner);
    REQUIRE(before);
    REQUIRE(before->activations());
    CHECK(*before->activations() == owner.activations());
    std::vector<PlacedEnemies::Placement> actual;
    // An empty-result dispatcher represents a full native pool. Activation is
    // still consumed, rather than retried on every later camera update.
    owner.activate([&](const auto& standing) {
        CHECK(standing.spawn.priority == EnemySpawn::Priority::FreeSlotOnly);
        actual.push_back(standing);
    });
    REQUIRE(actual.size() == roster.size());
    CHECK(*before->activations() == actual);
    CHECK(owner.count() == 0);
    owner.activate([&](const auto&) { FAIL("a consumed placement was retried"); });
    REQUIRE(PlacedEnemiesCheckpoint::restore(owner, *before));
    CHECK(owner.count() == roster.size());
    CHECK(owner.inView({100000, 0, -100000}, 1));
}

TEST_CASE("placed enemy checkpoint preserves negative authored default-way selectors",
          "[rollback][placed-enemies]") {
    auto record = placement({0, 0, 10});
    record.spawn.algorithm = -32768;
    PlacedEnemies owner;
    owner.bind({record});
    const auto state = PlacedEnemiesCheckpoint::capture(owner);
    REQUIRE(state);
    REQUIRE(state->valid());
    REQUIRE(PlacedEnemiesCheckpoint::restore(owner, *state));
    CHECK(owner.activations().front().spawn.algorithm == -32768);
}

TEST_CASE("placement value preflight matches prospective native watch without mutation",
          "[rollback][placed-enemies]") {
    PlacedEnemies owner;
    owner.bind({placement({0, 0, 10}), placement({0, 0, 100}, kDeathKind),
                placement({0, 0, 100}, kDeathKind)});
    owner.observe(ViewVolume{}, {});
    const auto before = PlacedEnemiesCheckpoint::capture(owner);
    REQUIRE(before);
    ViewVolume next;
    next.position = {0, 0, 80};
    const Vec3 attention{0, 0, 100};
    const auto prospective = before->activations(next, attention);
    REQUIRE(prospective);
    REQUIRE(prospective->size() == 2);
    CHECK(PlacedEnemiesCheckpoint::capture(owner) == before);
    owner.observe(next, attention);
    CHECK(owner.activations() == *prospective);
    std::vector<PlacedEnemies::Placement> born;
    owner.activate([&](const auto& value) { born.push_back(value); });
    CHECK(born == *prospective);

    auto malformed = *before;
    malformed.pending.push_back(malformed.roster.size());
    CHECK_FALSE(malformed.activations());
    next.position.x = std::numeric_limits<f32>::infinity();
    CHECK_FALSE(before->activations(next, attention));
    CHECK_FALSE(before->activations(std::nullopt, Vec3{std::numeric_limits<f32>::quiet_NaN()}));
    CHECK(before->activations()->size() == 1);
}

TEST_CASE("placed enemy checkpoint rejects malformed order view and placement data atomically",
          "[rollback][placed-enemies]") {
    PlacedEnemies owner;
    owner.bind({placement({0, 0, 10}), placement({0, 0, 100})});
    const auto before = PlacedEnemiesCheckpoint::capture(owner);
    REQUIRE(before);
    auto invalid = *before;
    SECTION("out of roster") {
        invalid.pending.push_back(2);
    }
    SECTION("duplicate membership") {
        invalid.pending = {0, 0};
    }
    SECTION("reordered membership") {
        invalid.pending = {1, 0};
    }
    SECTION("nonfinite attention") {
        invalid.attention.x = std::numeric_limits<f32>::quiet_NaN();
    }
    SECTION("nonfinite view") {
        invalid.view.emplace();
        invalid.view->up.y = std::numeric_limits<f32>::infinity();
    }
    SECTION("view angle bounds") {
        invalid.view.emplace();
        invalid.view->sinY = 2;
    }
    SECTION("spawn priority enum") {
        invalid.roster[0].spawn.priority = static_cast<EnemySpawn::Priority>(2);
    }
    SECTION("spawn timer") {
        invalid.roster[0].spawn.throwInterval = -1;
    }
    SECTION("kind") {
        invalid.roster[0].kind = kEnemyKindCount;
    }
    SECTION("carried item") {
        invalid.roster[0].carried = usize{1'000'001};
    }
    SECTION("placement bounds") {
        invalid.roster.resize(PlacedEnemiesCheckpoint::State::kMaxPlacements + 1,
                              placement({0, 0, 10}));
    }
    REQUIRE_FALSE(invalid.valid());
    CHECK_FALSE(PlacedEnemiesCheckpoint::prepareRestore(owner, invalid));
    CHECK_FALSE(PlacedEnemiesCheckpoint::restore(owner, invalid));
    CHECK(PlacedEnemiesCheckpoint::capture(owner) == before);
}

TEST_CASE("placed enemy restore resolves the entire immutable roster not only pending entries",
          "[rollback][placed-enemies]") {
    PlacedEnemies owner;
    owner.bind({placement({0, 0, 10}), placement({0, 0, 100})});
    owner.observe(ViewVolume{}, {});
    owner.activate([](const auto&) {});
    const auto before = PlacedEnemiesCheckpoint::capture(owner);
    REQUIRE(before);
    REQUIRE(before->pending == std::vector<usize>{1});
    auto wrong = *before;
    SECTION("consumed record") {
        wrong.roster[0].spawn.position.z += 1;
    }
    SECTION("pending spawn") {
        wrong.roster[1].spawn.sight += 1;
    }
    SECTION("carried binding") {
        wrong.roster[1].carried = 3;
    }
    SECTION("order") {
        std::swap(wrong.roster[0], wrong.roster[1]);
    }
    REQUIRE(wrong.valid());
    CHECK_FALSE(PlacedEnemiesCheckpoint::restore(owner, wrong));
    CHECK(PlacedEnemiesCheckpoint::capture(owner) == before);
    PlacedEnemies other;
    other.bind(before->roster);
    REQUIRE(PlacedEnemiesCheckpoint::restore(other, *before));
    CHECK(PlacedEnemiesCheckpoint::capture(other) == before);
}

TEST_CASE("placed enemy prepared restore is owner bound single-use and invalidated by mutation",
          "[rollback][placed-enemies]") {
    const std::vector roster{placement({0, 0, 10})};
    PlacedEnemies owner;
    PlacedEnemies other;
    owner.bind(roster);
    other.bind(roster);
    const auto before = PlacedEnemiesCheckpoint::capture(owner);
    REQUIRE(before);
    auto prepared = PlacedEnemiesCheckpoint::prepareRestore(owner, *before);
    REQUIRE(prepared);
    CHECK_FALSE(PlacedEnemiesCheckpoint::canCommit(other, *prepared));
    CHECK_FALSE(PlacedEnemiesCheckpoint::commitRestore(other, std::move(*prepared)));
    REQUIRE(PlacedEnemiesCheckpoint::canCommit(owner, *prepared));
    REQUIRE(PlacedEnemiesCheckpoint::commitRestore(owner, std::move(*prepared)));
    CHECK_FALSE(PlacedEnemiesCheckpoint::canCommit(owner, *prepared));
    CHECK_FALSE(PlacedEnemiesCheckpoint::commitRestore(owner, std::move(*prepared)));
    prepared = PlacedEnemiesCheckpoint::prepareRestore(owner, *before);
    REQUIRE(prepared);
    SECTION("rebind") {
        owner.bind(roster);
    }
    SECTION("clear") {
        owner.clear();
    }
    SECTION("watch") {
        owner.observe(ViewVolume{}, {});
    }
    SECTION("activation") {
        owner.activate([](const auto&) {});
    }
    const auto after = PlacedEnemiesCheckpoint::capture(owner);
    CHECK_FALSE(PlacedEnemiesCheckpoint::canCommit(owner, *prepared));
    CHECK_FALSE(PlacedEnemiesCheckpoint::commitRestore(owner, std::move(*prepared)));
    CHECK(PlacedEnemiesCheckpoint::capture(owner) == after);
}

TEST_CASE("placed enemy preparation cannot cross owner destruction at a reused address",
          "[rollback][placed-enemies]") {
    const std::vector roster{placement({0, 0, 10})};
    auto storage = std::make_unique<std::optional<PlacedEnemies>>();
    auto& first = storage->emplace();
    first.bind(roster);
    const auto state = PlacedEnemiesCheckpoint::capture(first);
    REQUIRE(state);
    auto prepared = PlacedEnemiesCheckpoint::prepareRestore(first, *state);
    REQUIRE(prepared);
    const auto* address = &first;
    storage->reset();
    auto& second = storage->emplace();
    CHECK(&second == address);
    second.bind(roster);
    CHECK_FALSE(PlacedEnemiesCheckpoint::canCommit(second, *prepared));
    CHECK_FALSE(PlacedEnemiesCheckpoint::commitRestore(second, std::move(*prepared)));
    CHECK(PlacedEnemiesCheckpoint::capture(second) == state);
}

TEST_CASE("diagnostic placement preparation uses content not legacy mutation counters",
          "[placed-enemies][diagnostics]") {
    PlacedEnemies owner;
    owner.bind({placement({0, 0, 100})});
    owner.observe(ViewVolume{}, {});
    const auto state = PlacedEnemiesCheckpoint::capture(owner);
    REQUIRE(state);
    auto prepared = PlacedEnemiesCheckpoint::prepareRestore(owner, *state);
    REQUIRE(prepared);
    owner.observe(ViewVolume{}, {});
    owner.activate([](const auto&) { FAIL("the distant placement should remain asleep"); });
    CHECK(PlacedEnemiesCheckpoint::canCommit(owner, *prepared));
    PlacedEnemies::LocalSnapshot baseline;
    owner.saveLocal(baseline);
    owner.observe(std::nullopt, {});
    CHECK_FALSE(PlacedEnemiesCheckpoint::canCommit(owner, *prepared));
    REQUIRE(owner.restoreLocal(baseline));
    CHECK(PlacedEnemiesCheckpoint::canCommit(owner, *prepared));
    REQUIRE(PlacedEnemiesCheckpoint::commitRestore(owner, std::move(*prepared)));
    CHECK_FALSE(owner.canRestoreLocal(baseline));
    CHECK(PlacedEnemiesCheckpoint::capture(owner) == state);
}

TEST_CASE("placed enemy native rewind survives watch and activation without repeating callbacks",
          "[placed-enemies-local-rewind]") {
    PlacedEnemies owner;
    PlacedEnemies other;
    const std::vector roster{placement({0, 0, 10}), placement({0, 0, 100})};
    owner.bind(roster);
    other.bind(roster);
    PlacedEnemies::LocalSnapshot saved;
    owner.saveLocal(saved);
    CHECK_FALSE(other.restoreLocal(saved));
    std::vector<PlacedEnemies::Placement> calls;
    owner.observe(ViewVolume{}, {1, 0, 0});
    owner.activate([&](const auto& record) { calls.push_back(record); });
    REQUIRE(calls.size() == 1);
    owner.observe(std::nullopt, {2, 0, 0});
    owner.activate([&](const auto& record) { calls.push_back(record); });
    REQUIRE(calls.size() == 2);
    owner.activate([&](const auto&) { FAIL("consumed placement must remain consumed"); });
    REQUIRE(owner.count() == 0);
    REQUIRE(owner.restoreLocal(saved));
    CHECK(calls.size() == 2);
    REQUIRE(owner.restoreLocal(saved)); // Native snapshots are reusable value history.
    owner.activate([&](const auto& record) { calls.push_back(record); });
    REQUIRE(calls.size() == 4);
    CHECK(calls[2].spawn.position == roster[0].spawn.position);
    CHECK(calls[3].spawn.position == roster[1].spawn.position);
}

TEST_CASE("placed enemy native rewind retains resource lifetime and replacement fences",
          "[placed-enemies-local-rewind]") {
    const s32 mutation = GENERATE(0, 1, 2, 3);
    const std::vector roster{placement({0, 0, 10})};
    auto storage = std::make_unique<std::optional<PlacedEnemies>>();
    auto& owner = storage->emplace();
    owner.bind(roster);
    const auto before = PlacedEnemiesCheckpoint::capture(owner);
    REQUIRE(before);
    PlacedEnemies::LocalSnapshot saved;
    owner.saveLocal(saved);
    switch (mutation) {
    case 0: owner.bind(roster); break;
    case 1: owner.clear(); break;
    case 2: REQUIRE(PlacedEnemiesCheckpoint::restore(owner, *before)); break;
    case 3:
        storage->reset();
        storage->emplace().bind(roster);
        break;
    default: break;
    }
    const auto after = PlacedEnemiesCheckpoint::capture(storage->value());
    CHECK_FALSE(storage->value().canRestoreLocal(saved));
    CHECK_FALSE(storage->value().restoreLocal(saved));
    CHECK(PlacedEnemiesCheckpoint::capture(storage->value()) == after);
}

TEST_CASE("placed native births preserve pool replacement failure queues and RNG after correction",
          "[rollback][placed-enemies][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 1, {}, 917);
    REQUIRE(enemies.loadKind(kGruntKind));
    auto existing = placement({0, 0, 20});
    existing.spawn.generator = 4;
    REQUIRE(enemies.spawn(existing.spawn, {}));
    ViewVolume view;
    enemies.setView(view);
    const auto baseline = enemies.checkpoint();
    REQUIRE(baseline);
    const auto uploads = device.texturesCreated;
    auto first = placement({0, 0, 10});
    first.spawn.algorithm = kWanderWay;
    auto second = placement({0, 0, 15});
    second.spawn.algorithm = kZigZagWay;
    const std::vector roster{first, second, placement({0, 0, 100})};
    PlacedEnemies owner;
    owner.bind(roster);
    const bool watched = GENERATE(false, true);
    const auto observation = watched ? std::optional<ViewVolume>{view} : std::nullopt;
    owner.observe(observation, {});
    const auto placementsBefore = PlacedEnemiesCheckpoint::capture(owner);
    REQUIRE(placementsBefore);
    auto native = roster;
    std::vector<std::optional<s32>> expectedSlots;
    originalPhase(native, observation, {}, [&](const auto& record) {
        expectedSlots.push_back(enemies.spawn(record.spawn, {}));
    });
    const auto expected = enemies.checkpoint();
    REQUIRE(expected);
    if (watched) {
        REQUIRE(expectedSlots.size() == 2);
        CHECK(expectedSlots[0].has_value());
        CHECK(expectedSlots[1].has_value());
        CHECK(expected->nextInstance > baseline->nextInstance);
        CHECK(expected->random != baseline->random);
        CHECK(expected->generatorEvents.size() > baseline->generatorEvents.size());
    } else {
        REQUIRE(expectedSlots.size() == 3);
        CHECK(
            std::ranges::none_of(expectedSlots, [](const auto& slot) { return slot.has_value(); }));
        CHECK(expected == baseline);
    }
    REQUIRE(enemies.restore(*baseline));
    std::vector<std::optional<s32>> actualSlots;
    owner.activate(
        [&](const auto& record) { actualSlots.push_back(enemies.spawn(record.spawn, {})); });
    CHECK(actualSlots == expectedSlots);
    CHECK(enemies.checkpoint() == expected);
    CHECK(pendingRecords(owner) == native);
    const auto placementsAfter = PlacedEnemiesCheckpoint::capture(owner);
    REQUIRE(placementsAfter);
    // Delayed correction restores both owners; placement restore itself never
    // spawns or consumes the native random engine.
    REQUIRE(enemies.restore(*baseline));
    REQUIRE(PlacedEnemiesCheckpoint::restore(owner, *placementsBefore));
    CHECK(enemies.checkpoint() == baseline);
    owner.activate([&](const auto& record) { enemies.spawn(record.spawn, {}); });
    CHECK(enemies.checkpoint() == expected);
    CHECK(PlacedEnemiesCheckpoint::capture(owner) == placementsAfter);
    CHECK(device.texturesCreated == uploads);
}

TEST_CASE("real G1 retains and restores its complete native pending placement roster",
          "[rollback][placed-enemies][level-opponents][assets]") {
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
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, {}, 0);
    LevelOpponents::Resources resources{device, world, weapons, effects, audio, root, 1};
    resources.standOnSight = true;
    opponents.open(resources, players);
    const auto initial = PlacedEnemiesCheckpoint::capture(opponents.placements());
    const auto enemyBefore = opponents.enemies().checkpoint();
    REQUIRE(initial);
    REQUIRE(enemyBefore);
    REQUIRE_FALSE(initial->pending.empty());
    ViewVolume overhead;
    overhead.forward = {0, -1, 0};
    overhead.up = {0, 0, 1};
    std::optional<Vec3> spot;
    PlacedEnemies probe;
    probe.bind(initial->roster);
    REQUIRE(PlacedEnemiesCheckpoint::restore(probe, *initial));
    for (const usize index : initial->pending) {
        const auto& candidate = initial->roster[index];
        overhead.position = candidate.spawn.position + Vec3{0, 10, 0};
        probe.observe(overhead, candidate.spawn.position);
        const auto births = probe.activations();
        // This oracle restores the ordinary pool only. Do not mutate a great
        // one's independent pool while comparing a rewound ordinary encounter.
        if (!births.empty() && std::ranges::all_of(births, [](const auto& record) {
                return record.kind < kSwarmKindCount && record.kind != kGarmBroodKind;
            })) {
            spot = candidate.spawn.position;
            break;
        }
    }
    REQUIRE(spot);
    const auto uploads = device.texturesCreated;
    opponents.watch(overhead, *spot);
    const auto expected = PlacedEnemiesCheckpoint::capture(opponents.placements());
    const auto enemyAfter = opponents.enemies().checkpoint();
    REQUIRE(expected);
    REQUIRE(enemyAfter);
    CHECK(expected->pending.size() < initial->pending.size());
    CHECK(expected->roster == initial->roster);
    REQUIRE(PlacedEnemiesCheckpoint::restore(opponents.placements(), *initial));
    REQUIRE(opponents.enemies().restore(*enemyBefore));
    opponents.watch(overhead, *spot);
    CHECK(PlacedEnemiesCheckpoint::capture(opponents.placements()) == expected);
    CHECK(opponents.enemies().checkpoint() == enemyAfter);
    CHECK(device.texturesCreated == uploads);
}

TEST_CASE("native placed activation resumes the same enemy simulation after a delayed correction",
          "[rollback][placed-enemies][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 2, {}, 514);
    REQUIRE(enemies.loadKind(kGruntKind));
    enemies.setContactGridBounds({-100, -10, -100}, {100, 20, 100});
    const auto initial = enemies.checkpoint();
    REQUIRE(initial);
    const std::vector roster{placement({-40, 0, 12}), placement({0, 0, 12}),
                             placement({40, 0, 12})};
    auto pending = roster;
    std::vector<Enemies::Checkpoint> forward;
    std::vector<std::vector<PlacedEnemies::Placement>> waiting;
    const auto uploads = device.texturesCreated;
    const auto observation = [](s32 tick) {
        ViewVolume view;
        // NOLINTNEXTLINE(bugprone-integer-division): integer division picks the grid row
        view.position.x = -40 + static_cast<f32>(tick / 12) * 20;
        return view;
    };
    const auto advance = [&](const ViewVolume& view) {
        const std::array players{EnemyView{.player = 0, .position = view.position}};
        enemies.update(1, 1.0f / 60, players, {});
    };
    for (s32 tick = 0; tick < 54; ++tick) {
        const auto view = observation(tick);
        enemies.setView(view);
        originalPhase(pending, view, view.position,
                      [&](const auto& record) { enemies.spawn(record.spawn, {}); });
        advance(view);
        const auto state = enemies.checkpoint();
        REQUIRE(state);
        forward.push_back(*state);
        waiting.push_back(pending);
    }
    REQUIRE(pending.empty());
    CHECK(forward.back().nextInstance == 4);
    CHECK(forward.back().random != initial->random);
    REQUIRE(enemies.restore(*initial));
    PlacedEnemies owner;
    owner.bind(roster);
    std::optional<PlacedEnemiesCheckpoint::State> delayed;
    for (s32 tick = 0; tick < 54; ++tick) {
        CAPTURE(tick);
        const auto view = observation(tick);
        enemies.setView(view);
        owner.observe(view, view.position);
        owner.activate([&](const auto& record) { enemies.spawn(record.spawn, {}); });
        advance(view);
        CHECK(enemies.checkpoint() == forward[static_cast<usize>(tick)]);
        CHECK(pendingRecords(owner) == waiting[static_cast<usize>(tick)]);
        if (tick == 17) {
            delayed = PlacedEnemiesCheckpoint::capture(owner);
            REQUIRE(delayed);
        }
    }
    REQUIRE(delayed);
    REQUIRE(PlacedEnemiesCheckpoint::restore(owner, *delayed));
    REQUIRE(enemies.restore(forward[17]));
    for (s32 tick = 18; tick < 54; ++tick) {
        CAPTURE(tick);
        const auto view = observation(tick);
        enemies.setView(view);
        owner.observe(view, view.position);
        owner.activate([&](const auto& record) { enemies.spawn(record.spawn, {}); });
        advance(view);
        CHECK(enemies.checkpoint() == forward[static_cast<usize>(tick)]);
        CHECK(pendingRecords(owner) == waiting[static_cast<usize>(tick)]);
    }
    CHECK(device.texturesCreated == uploads);
}

TEST_CASE("native placement rewind replays corrected watch branches without spawning on restore",
          "[placed-enemies-local-rewind][native-equality]") {
    PlacedEnemies reference;
    PlacedEnemies predicted;
    std::vector<PlacedEnemies::Placement> roster;
    roster.reserve(40);
    for (s32 row = 0; row < 40; ++row) {
        roster.push_back(placement({static_cast<f32>(row * 20), 0, 10}));
    }
    reference.bind(roster);
    predicted.bind(roster);
    PlacedEnemies::LocalSnapshot saved;
    std::array<PlacedEnemies::LocalSnapshot, 8> expected;
    PlacedEnemies::LocalSnapshot actual;
    usize callbacks = 0;
    const auto advance = [&](PlacedEnemies& owner, s32 tick) {
        ViewVolume view;
        // NOLINTNEXTLINE(bugprone-integer-division): integer division picks the grid row
        view.position = {static_cast<f32>((tick / 4) * 20), 0, 0};
        owner.observe(view, view.position);
        std::vector<PlacedEnemies::Placement> born;
        owner.activate([&](const auto& p) {
            ++callbacks;
            born.push_back(p);
        });
        return born;
    };
    s32 tick = 0;
    for (s32 window = 0; window < 32; ++window) {
        CAPTURE(window, tick);
        const auto before = PlacedEnemiesCheckpoint::capture(predicted);
        REQUIRE(before);
        predicted.saveLocal(saved);
        const s32 distance = 1 + window % 8;
        for (s32 step = 0; step < distance; ++step) {
            advance(predicted, tick + step);
            predicted.saveLocal(expected[static_cast<usize>(step)]);
        }
        REQUIRE(predicted.restoreLocal(saved));
        auto diagnostic = PlacedEnemiesCheckpoint::prepareRestore(predicted, *before);
        REQUIRE(diagnostic);
        predicted.observe(std::nullopt, {-900, 0, 0});
        predicted.activate([&](const auto&) { ++callbacks; });
        const auto calls = callbacks;
        REQUIRE(predicted.restoreLocal(saved));
        CHECK(callbacks == calls);
        // The test-only preparation guard compares content, not native mutation counts.
        CHECK(PlacedEnemiesCheckpoint::canCommit(predicted, *diagnostic));
        CHECK(PlacedEnemiesCheckpoint::capture(predicted) == before);
        for (s32 step = 0; step < distance; ++step, ++tick) {
            CHECK(advance(predicted, tick) == advance(reference, tick));
            predicted.saveLocal(actual);
            CHECK(predicted.equalLocal(expected[static_cast<usize>(step)], actual));
            CHECK(PlacedEnemiesCheckpoint::capture(predicted) ==
                  PlacedEnemiesCheckpoint::capture(reference));
        }
        REQUIRE(predicted.canRestoreLocal(saved));
    }
    CHECK(reference.count() < roster.size());
    CHECK(callbacks > 0);
}

TEST_CASE("native placement histories are invalidated only by roster or diagnostic replacement",
          "[placed-enemies-local-rewind][native-equality]") {
    PlacedEnemies owner;
    PlacedEnemies foreign;
    const std::vector roster{placement({0, 0, 10})};
    owner.bind(roster);
    foreign.bind(roster);
    PlacedEnemies::LocalSnapshot saved;
    CHECK_FALSE(owner.equalLocal(saved, saved));
    CHECK_FALSE(owner.restoreLocal(saved));
    owner.saveLocal(saved);
    CHECK(owner.equalLocal(saved, saved));
    CHECK_FALSE(foreign.equalLocal(saved, saved));
    CHECK_FALSE(foreign.restoreLocal(saved));
    REQUIRE(owner.restoreLocal(saved));
    REQUIRE(owner.restoreLocal(saved));
    REQUIRE(PlacedEnemiesCheckpoint::restore(owner, *PlacedEnemiesCheckpoint::capture(owner)));
    CHECK_FALSE(owner.equalLocal(saved, saved));
    CHECK_FALSE(owner.restoreLocal(saved));
    owner.saveLocal(saved);
    owner.bind(roster);
    CHECK_FALSE(owner.equalLocal(saved, saved));
    CHECK_FALSE(owner.restoreLocal(saved));
    owner.saveLocal(saved);
    owner.clear();
    CHECK_FALSE(owner.equalLocal(saved, saved));
    CHECK_FALSE(owner.restoreLocal(saved));
}

TEST_CASE("native placement equality includes every view field and unseen attention",
          "[placed-enemies-local-rewind][native-equality]") {
    PlacedEnemies owner;
    owner.bind({placement({0, 0, 100})});
    ViewVolume view;
    owner.observe(view, {});
    PlacedEnemies::LocalSnapshot before;
    PlacedEnemies::LocalSnapshot after;
    owner.saveLocal(before);
    const s32 change = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
    Vec3 attention{0};
    switch (change) {
    case 0: view.position.x += 1; break;
    case 1: view.right.x += 0.01f; break;
    case 2: view.up.y += 0.01f; break;
    case 3: view.forward.z += 0.01f; break;
    case 4: view.sinX += 0.01f; break;
    case 5: view.cosX -= 0.01f; break;
    case 6: view.sinY += 0.01f; break;
    case 7: view.cosY -= 0.01f; break;
    case 8: attention.x = 1; break;
    default: break;
    }
    owner.observe(change == 9 ? std::nullopt : std::optional{view}, attention);
    owner.saveLocal(after);
    CHECK_FALSE(owner.equalLocal(before, after));
    CHECK(owner.count() == 1);
    REQUIRE(owner.restoreLocal(before));
    owner.saveLocal(after);
    CHECK(owner.equalLocal(before, after));
}

} // namespace
