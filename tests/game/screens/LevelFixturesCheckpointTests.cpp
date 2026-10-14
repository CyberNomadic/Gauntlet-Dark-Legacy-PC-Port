#include <algorithm>
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/LevelFixturesCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;
struct Fixture {
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelFixtures fixtures;
    LevelFixturesCheckpoint owner{fixtures};
    std::array<PlayerRuntime, 2> players;
    std::vector<std::pair<usize, f32>> hits;
    LevelFixtures::Events events{
        .hurt = [&](usize index, f32 damage, HurtKind, bool,
                    const PlayerImpact&) { hits.emplace_back(index, damage); },
        .opponents =
            [](const Vec3&, f32, f32, std::vector<s32>& reached, u32) {
                if (std::ranges::find(reached, 17) == reached.end()) {
                    reached.push_back(17);
                }
            }};
    Fixture() {
        bind();
        players[0].actor.spawn(3, {}, nullptr, {0, 0, 0}, 0);
        players[1].actor.spawn(1, {}, nullptr, {6, 0, 0}, 0);
    }
    void bind() {
        fixtures.bind({device, world, weapons, effects, audio, 1});
        REQUIRE(owner.bindResources());
    }
};
TEST_CASE("fixture aggregate rewinds complete delayed blast contacts without duplicate output",
          "[rollback][level-fixtures-checkpoint]") {
    const auto rate = GENERATE(30, 60);
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    Fixture first, second;
    const auto empty = first.owner.checkpoint();
    REQUIRE(empty);
    first.fixtures.blast({0, 0, 0}, 12, 30, first.players, first.events);
    REQUIRE(first.hits.size() == 1);
    const auto state = first.owner.checkpoint();
    REQUIRE(state);
    REQUIRE(state->blasts.size() == 1);
    REQUIRE(state->blasts[0].players.size() == 1);
    const auto textures = second.device.texturesCreated;
    REQUIRE(second.owner.restore(*state));
    CHECK(second.hits.empty());
    CHECK(second.owner.checkpoint() == state);
    first.hits.clear();
    for (s32 tick = 0; tick < rate; ++tick) {
        first.fixtures.advanceBlasts(seconds, first.players, first.events);
        second.fixtures.advanceBlasts(seconds, second.players, second.events);
        CHECK(second.owner.checkpoint() == first.owner.checkpoint());
        CHECK(second.hits == first.hits);
    }
    REQUIRE(first.hits.size() == 1);
    CHECK(first.hits[0].first == 1);
    CHECK(second.device.texturesCreated == textures);
    auto undo = second.owner.prepareRestore(*empty);
    REQUIRE(undo);
    second.fixtures.blast({0, 0, 0}, 12, 30, second.players, second.events);
    const auto calls = second.hits.size();
    REQUIRE(second.owner.commitRestore(std::move(*undo)));
    CHECK(second.owner.checkpoint() == empty);
    CHECK(second.hits.size() == calls);
}
TEST_CASE("fixture aggregate preflights every child and invalidates stale siblings",
          "[rollback][level-fixtures-checkpoint]") {
    Fixture fixture;
    const auto state = fixture.owner.checkpoint();
    REQUIRE(state);
    for (const int mutation : {0, 1, 2, 3, 4}) {
        auto invalid = *state;
        switch (mutation) {
        case 0: invalid.traps.random = 0; break;
        case 1: invalid.rubble.resources.push_back({0, "NOT_BOUND"}); break;
        case 2: invalid.doomedChests.push_back(0); break;
        case 3:
            invalid.clouds.push_back({{0, 0, 0}, 1, std::numeric_limits<f32>::quiet_NaN(), 0});
            break;
        case 4: invalid.difficultyGain = 2; break;
        }
        CHECK_FALSE(fixture.owner.restore(invalid));
        CHECK(fixture.owner.checkpoint() == state);
        CHECK(fixture.hits.empty());
    }
    auto stale = fixture.owner.prepareRestore(*state);
    REQUIRE(stale);
    ChestsCheckpoint child{fixture.fixtures.chests()};
    REQUIRE(child.restore(state->chests));
    CHECK_FALSE(fixture.owner.canCommit(*stale));
    CHECK_FALSE(fixture.owner.commitRestore(std::move(*stale)));
    CHECK(fixture.owner.checkpoint() == state);
    stale = fixture.owner.prepareRestore(*state);
    REQUIRE(stale);
    LevelFixturesCheckpoint sibling{fixture.fixtures};
    REQUIRE(sibling.bindResources());
    REQUIRE(sibling.restore(*state));
    CHECK_FALSE(fixture.owner.canCommit(*stale));
    stale = fixture.owner.prepareRestore(*state);
    REQUIRE(stale);
    fixture.bind();
    CHECK_FALSE(fixture.owner.canCommit(*stale));
    CHECK(fixture.owner.checkpoint() == state);
}

TEST_CASE("fixture capture and full validation agree without trusting incoming child states",
          "[rollback][level-fixtures-checkpoint][checkpoint-capture-validation]") {
    Fixture fixture;
    fixture.fixtures.blast({0, 0, 0}, 12, 30, fixture.players, fixture.events);
    const auto baseline = fixture.owner.checkpoint();
    REQUIRE(baseline);
    REQUIRE(baseline->valid());
    LevelFixturesCheckpoint independent{fixture.fixtures};
    REQUIRE(independent.bindResources());
    CHECK(independent.checkpoint() == baseline);
    for (s32 mutation = 0; mutation < 7; ++mutation) {
        CAPTURE(mutation);
        auto invalid = *baseline;
        switch (mutation) {
        case 0:
            invalid.chests.chests.emplace_back().refusalLeft =
                std::numeric_limits<f32>::quiet_NaN();
            break;
        case 1:
            invalid.gates.gates.emplace_back().refusalLeft = std::numeric_limits<f32>::quiet_NaN();
            break;
        case 2: invalid.traps.random = 0; break;
        case 3: invalid.barrels.barrels.emplace_back(); break;
        case 4: invalid.rocks.rocks.emplace_back(); break;
        case 5: invalid.scenery.props.emplace_back(); break;
        case 6:
            invalid.rubble.pieces.push_back(
                {static_cast<u32>(invalid.rubble.resources.size()), Mat4{1}});
            break;
        }
        CHECK_FALSE(invalid.valid());
        CHECK_FALSE(fixture.owner.prepareRestore(invalid));
        CHECK(fixture.owner.checkpoint() == baseline);
    }
    const auto emitted = fixture.hits.size();
    CHECK(independent.checkpoint() == fixture.owner.checkpoint());
    CHECK(fixture.hits.size() == emitted);
}

TEST_CASE(
    "fixture cooldown validation accepts unordered bounded ledgers and rejects bad references",
    "[rollback][level-fixtures-checkpoint][fixture-timers]") {
    Fixture fixture;
    const auto empty = fixture.owner.checkpoint();
    REQUIRE(empty);
    auto state = *empty;
    LevelFixturesCheckpoint::Blast blast;
    blast.radius = 12;
    blast.damage = 30;
    blast.seconds = 3;
    blast.flags = 0x800;
    blast.started = true;
    blast.players = {3, 0, 2, 1};
    blast.playerReady = {{2, 0.5f}, {0, 0.75f}, {1, 0.0f}, {3, 1.0f}};
    // Preserve the actual aggregate contact bound, including non-contiguous
    // opponent identities and contact/timer orders that differ from each other.
    constexpr s32 kContacts = 65535;
    blast.opponents.reserve(kContacts);
    blast.opponentReady.reserve(kContacts);
    for (s32 index = 0; index < kContacts; ++index) {
        blast.opponents.push_back((kContacts - index) * 2);
        blast.opponentReady.emplace_back((index + 1) * 2, 0.5f);
    }
    state.blasts.push_back(std::move(blast));
    REQUIRE(state.valid());
    REQUIRE(fixture.owner.restore(state));
    CHECK(fixture.owner.checkpoint() == state);
    CHECK(fixture.hits.empty());
    for (const s32 mutation : {0, 1, 2, 3, 4, 5, 6, 7}) {
        CAPTURE(mutation);
        auto invalid = state;
        auto& ring = invalid.blasts[0];
        switch (mutation) {
        case 0: ring.opponentReady[0].first = 1; break; // Not an admitted contact.
        case 1: ring.opponentReady[1].first = ring.opponentReady[0].first; break;
        case 2: ring.opponentReady[0].second = std::numeric_limits<f32>::quiet_NaN(); break;
        case 3: ring.opponentReady[0].second = -0.1f; break;
        case 4: ring.playerReady[0].first = 4; break;
        case 5: ring.playerReady[1].first = ring.playerReady[0].first; break;
        case 6: ring.opponentReady.emplace_back(2, 0.5f); break;
        case 7: ring.reached.push_back(1); break; // Aggregate contact bound exceeded.
        }
        CHECK_FALSE(invalid.valid());
        CHECK_FALSE(fixture.owner.prepareRestore(invalid));
        CHECK(fixture.owner.checkpoint() == state);
        CHECK(fixture.hits.empty());
    }
}

TEST_CASE("fixture poison cooldown replay preserves expiry order and repeated native hits",
          "[rollback][level-fixtures-checkpoint][fixture-timers]") {
    const s32 rate = GENERATE(30, 60);
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    Fixture first, second;
    auto state = first.owner.checkpoint();
    REQUIRE(state);
    // This is an incoming authoritative poison-ring checkpoint. The production
    // blast phase below, not a test-side timer simulation, expires and refills it.
    LevelFixturesCheckpoint::Blast ring;
    ring.radius = 12;
    ring.damage = 30;
    ring.seconds = 3;
    ring.elapsed = 0.25f;
    ring.flags = 0x800;
    ring.started = true;
    ring.players = {0};
    ring.opponents = {17};
    ring.playerReady = {{0, 0.5f}};
    ring.opponentReady = {{17, 0.5f}};
    state->blasts.push_back(ring);
    REQUIRE(first.owner.restore(*state));
    REQUIRE(second.owner.restore(*state));
    const auto textures = second.device.texturesCreated;
    usize repeatedHits = 0;
    usize refreshedOpponents = 0;
    for (s32 tick = 0; tick < rate; ++tick) {
        CAPTURE(rate, tick);
        first.hits.clear();
        second.hits.clear();
        const auto before = first.owner.checkpoint();
        REQUIRE(before);
        auto undo = second.owner.prepareRestore(*before);
        REQUIRE(undo);
        first.fixtures.advanceBlasts(seconds, first.players, first.events);
        second.fixtures.advanceBlasts(seconds, second.players, second.events);
        CHECK(second.owner.checkpoint() == first.owner.checkpoint());
        CHECK(second.hits == first.hits);
        repeatedHits += static_cast<usize>(
            std::ranges::count(first.hits, usize{0}, &std::pair<usize, f32>::first));
        const auto after = first.owner.checkpoint();
        REQUIRE(after);
        REQUIRE(after->blasts.size() == 1);
        REQUIRE(after->blasts[0].opponentReady.size() == 1);
        refreshedOpponents +=
            after->blasts[0].opponentReady[0].second > before->blasts[0].opponentReady[0].second
                ? 1U
                : 0U;
        if (tick + 1 < rate / 4) {
            CHECK(std::ranges::count(first.hits, usize{0}, &std::pair<usize, f32>::first) == 0);
        }
        const auto emitted = second.hits.size();
        REQUIRE(second.owner.commitRestore(std::move(*undo)));
        CHECK(second.owner.checkpoint() == before);
        CHECK(second.hits.size() == emitted); // Restore itself emits nothing.
        // The owning caller discards speculative output before replay instead
        // of delivering the same accepted tick twice.
        second.hits.clear();
        second.fixtures.advanceBlasts(seconds, second.players, second.events);
        CHECK(second.owner.checkpoint() == first.owner.checkpoint());
        CHECK(second.hits == first.hits);
    }
    CHECK(repeatedHits >= 2);
    CHECK(refreshedOpponents >= 2);
    CHECK(second.device.texturesCreated == textures);
}

TEST_CASE("native fixture aggregate retains all seven rosters and restores without uploads",
          "[rollback][level-fixtures-checkpoint][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const auto stage = GENERATE("G1", "F1", "J4");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName(stage);
    REQUIRE(level);
    Fixture first, second;
    first.fixtures.clear();
    second.fixtures.clear();
    REQUIRE(first.world.load(first.device, root, *level));
    REQUIRE(second.world.load(second.device, root, *level));
    first.bind();
    second.bind();
    first.fixtures.setPlayerCount(3);
    const auto state = first.owner.checkpoint();
    REQUIRE(state);
    CHECK(state->chests.chests.size() == first.fixtures.chests().size());
    CHECK(state->gates.gates.size() == first.fixtures.gates().size());
    CHECK(state->traps.traps.size() == first.fixtures.traps().size());
    CHECK(state->barrels.barrels.size() == first.fixtures.barrels().size());
    CHECK(state->rocks.rocks.size() == first.fixtures.safeRocks().size());
    CHECK(state->scenery.props.size() == first.fixtures.scenery().size());
    CHECK(state->rubble.pieces.size() == first.fixtures.rubble().size());
    const auto textures = second.device.texturesCreated, updates = second.device.textureUpdates;
    REQUIRE(second.owner.restore(*state));
    CHECK(second.owner.checkpoint() == state);
    CHECK(second.device.texturesCreated == textures);
    CHECK(second.device.textureUpdates == updates);
    CHECK(second.hits.empty());
}
} // namespace
