#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "../world/ProjectileCheckpointFixture.h"
#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/Generators.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr f32 kStep = 1.0f / 60;

std::filesystem::path nativeRoot() {
    return test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
}

WorldLayout layoutOf(std::string_view name, std::string_view text) {
    const auto directory = test::scratchDirectory(name);
    writeTextFile(directory / "world.json", text);
    WorldLayout layout;
    REQUIRE(layout.load(directory));
    return layout;
}

struct Pool {
    test::FakeRenderDevice device;
    WorldLayout layout;
    Enemies enemies;
    Generators generators;
    std::array<EnemyView, 1> players{EnemyView{.player = 0, .position = {0, 0, 25}}};

    Pool() {
        layout = layoutOf("generator-checkpoint", R"({
          "objects":[{"name":"GROUND","position":[0,0,0]}],
          "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,
                         "collisionType":1,"hitPoints":10}],
          "itemInstances":[
            {"info":0,"minPlayers":1,"position":[0,0,0],
             "params":[2,0,7,0,2,0,1,0,0,0,0,0]},
            {"info":0,"minPlayers":1,"position":[30,0,0],
             "params":[1,0,15,0,1,0,1,0,0,0,0,0]},
            {"info":0,"minPlayers":2,"position":[60,0,0],
             "params":[3,0,7,0,2,0,1,0,0,0,0,0]}]})");
        enemies.open(device, nativeRoot(), nullptr, 8, {}, 347);
        REQUIRE(generators.bind(device, layout, enemies, nullptr, {1.5f, 1.25f, 1.5f}, 1));
        REQUIRE(generators.count() == 3);
    }

    void step(s32 tick) {
        players[0].position.x = static_cast<f32>(tick % 11) * 0.1f;
        if (tick == 8 || tick == 24) {
            EnemyHit hit;
            hit.player = 0;
            hit.damage = 1000;
            enemies.hurt(0, hit);
        }
        if (tick == 14 || tick == 43) {
            REQUIRE(generators.strike(0, tick == 14 ? 20.0f : 100.0f, 0));
        }
        generators.setPlayerCount(tick >= 12 && tick < 30 ? 2 : 1);
        if (tick >= 32 && tick < 38) {
            ViewVolume view;
            view.position = {10000, 0, -20};
            generators.setView(view, Vec3{10000, 0, 0});
        } else {
            generators.setView(std::nullopt);
        }
        const bool stopped = tick >= 18 && tick < 23;
        // This is the real LevelOpponents phase order. Enemy output from the last
        // phase remains queued until the next generator step consumes it.
        generators.update(1, enemies, players, {}, stopped);
        enemies.update(1, kStep, players, generators.enemyObstacles(), nullptr, 1, stopped);
    }
};

TEST_CASE("generator and enemy pools rewind quotas timers presence textures and pending broods",
          "[rollback][generators][generators-checkpoint][assets]") {
    Pool pool;
    const auto initial = pool.generators.checkpoint();
    const auto initialEnemies = pool.enemies.checkpoint();
    REQUIRE(initial);
    REQUIRE(initialEnemies);
    CHECK(initial->generators[2].presence == Generators::Slot::Presence::Hidden);
    const auto uploads = pool.device.texturesCreated;
    std::vector<Generators::Checkpoint> states;
    std::vector<Enemies::Checkpoint> opponents;
    for (s32 tick = 0; tick < 72; ++tick) {
        pool.step(tick);
        const auto current = pool.generators.checkpoint();
        const auto enemyState = pool.enemies.checkpoint();
        REQUIRE(current);
        REQUIRE(enemyState);
        states.push_back(*current);
        opponents.push_back(*enemyState);
    }
    CHECK(states.front().generators[2].presence == Generators::Slot::Presence::Suppressed);
    CHECK(states.back().generators[0].state == 0);
    CHECK(states[14].generators[0].most == 6);
    CHECK(states.back().bodies.front().textures.frame > initial->bodies.front().textures.frame);
    CHECK(states.back().generators[0].bred > 0);
    CHECK(opponents.back().nextInstance > initialEnemies->nextInstance);
    REQUIRE(pool.enemies.restore(*initialEnemies));
    REQUIRE(pool.generators.restore(*initial));
    CHECK(pool.generators.checkpoint() == initial);
    for (s32 tick = 0; tick < 72; ++tick) {
        CAPTURE(tick);
        pool.step(tick);
        CHECK(pool.generators.checkpoint() == states[static_cast<usize>(tick)]);
        CHECK(pool.enemies.checkpoint() == opponents[static_cast<usize>(tick)]);
    }
    // A delayed correction in the middle of time-stop must resume the same
    // partially elapsed quota/texture clocks, not only a pristine level start.
    REQUIRE(pool.enemies.restore(opponents[20]));
    REQUIRE(pool.generators.restore(states[20]));
    for (s32 tick = 21; tick < 72; ++tick) {
        CAPTURE(tick);
        pool.step(tick);
        CHECK(pool.generators.checkpoint() == states[static_cast<usize>(tick)]);
        CHECK(pool.enemies.checkpoint() == opponents[static_cast<usize>(tick)]);
    }
    CHECK(pool.device.texturesCreated == uploads);
}

TEST_CASE("native generator history rewinds broods quotas presence and linked enemy output",
          "[local-rewind][generator-local-rewind][native-equality][assets]") {
    Pool pool;
    struct Frame {
        Generators::LocalSnapshot generators;
        Enemies::LocalSnapshot enemies;
    };
    std::array<Frame, 16> history;
    const auto step = [&](s32 tick, bool wrong) {
        pool.players[0].position = {wrong ? -80.0f : static_cast<f32>(tick % 13), 0, 25};
        pool.generators.setPlayerCount(wrong ? 4 : 1 + (tick / 23) % 2);
        pool.generators.setView(std::nullopt);
        if (tick % 43 == 0) {
            (void)pool.generators.strike((tick / 43) % 3, wrong ? 500.0f : 2.0f, 0);
        }
        if (tick % 29 == 0 && pool.enemies.count() > 0) {
            EnemyHit hit;
            hit.damage = wrong ? 5.0f : 1000.0f;
            hit.player = 0;
            (void)pool.enemies.hurt(0, hit);
        }
        const bool stopped = wrong || tick % 31 < 6;
        pool.generators.update(1, pool.enemies, pool.players, {}, stopped);
        pool.enemies.update(1, kStep, pool.players, pool.generators.enemyObstacles(), nullptr, 1,
                            stopped);
    };
    s32 tick = 0;
    for (usize window = 0; window < 96; ++window) {
        CAPTURE(window, tick);
        auto& saved = history[window % history.size()];
        pool.generators.saveLocal(saved.generators);
        REQUIRE(pool.enemies.saveLocal(saved.enemies));
        const s32 distance = static_cast<s32>(window % 8 + 1);
        std::vector<Generators::Checkpoint> generators;
        std::vector<Enemies::Checkpoint> enemies;
        std::vector<Frame> nativeExpected(static_cast<usize>(distance));
        for (s32 i = 0; i < distance; ++i) {
            step(tick + i, false);
            const auto g = pool.generators.checkpoint();
            const auto e = pool.enemies.checkpoint();
            REQUIRE(g);
            REQUIRE(e);
            generators.push_back(*g);
            enemies.push_back(*e);
            pool.generators.saveLocal(nativeExpected[static_cast<usize>(i)].generators);
            REQUIRE(pool.enemies.saveLocal(nativeExpected[static_cast<usize>(i)].enemies));
        }
        REQUIRE(pool.generators.restoreLocal(saved.generators));
        REQUIRE(pool.enemies.restoreLocal(saved.enemies));
        for (s32 i = 0; i < distance; ++i) {
            step(tick + i, true);
        }
        REQUIRE(pool.generators.restoreLocal(saved.generators));
        REQUIRE(pool.enemies.restoreLocal(saved.enemies));
        for (s32 i = 0; i < distance; ++i) {
            step(tick + i, false);
            CHECK(pool.generators.checkpoint() == generators[static_cast<usize>(i)]);
            CHECK(pool.enemies.checkpoint() == enemies[static_cast<usize>(i)]);
            Frame corrected;
            pool.generators.saveLocal(corrected.generators);
            REQUIRE(pool.enemies.saveLocal(corrected.enemies));
            CHECK(pool.generators.equalLocal(corrected.generators,
                                             nativeExpected[static_cast<usize>(i)].generators));
            CHECK(pool.enemies.equalLocal(corrected.enemies,
                                          nativeExpected[static_cast<usize>(i)].enemies));
        }
        tick += distance;
    }
}

TEST_CASE(
    "native generator history includes boss births animated landings and cancelled destruction",
    "[local-rewind][generator-local-rewind][native-equality][assets]") {
    Pool pool;
    test::ProjectileCheckpointFixture art;
    ItemInfo info;
    info.type = ItemInfo::kGenerator;
    info.name = "BOSSGEN";
    info.hitPoints = 20;
    info.radius = 2;
    info.height = 5;
    const bool settled = GENERATE(false, true);
    const auto observe = [&] {
        std::vector<Generators::Slot> slots;
        std::vector<std::optional<ItemFigure::Checkpoint>> figures;
        for (usize i = 0; i < pool.generators.count(); ++i) {
            slots.push_back(pool.generators.slot(i));
        }
        for (const auto& item : pool.generators.presentation()) {
            figures.push_back(item.bossFigure ? item.bossFigure->checkpoint() : std::nullopt);
        }
        return std::tuple{slots, figures, pool.enemies.checkpoint()};
    };
    const auto step = [&](s32 tick, bool wrong) {
        if (tick % 31 == 0 || (wrong && tick % 5 == 0)) {
            REQUIRE(pool.generators.placeBoss(
                pool.device, info, art.archive, pool.enemies, kGruntKind,
                glm::translate(Mat4{1}, Vec3{static_cast<f32>(tick), wrong ? 9.0f : 0.0f, 10}),
                nullptr, "FLIGHT", settled));
        }
        if (tick % 7 == 0 && pool.generators.count() > 3) {
            (void)pool.generators.strike(static_cast<s32>(pool.generators.count() - 1),
                                         wrong ? 1000.0f : 1.0f, 0);
        }
        pool.generators.update(1, pool.enemies, pool.players, {}, wrong);
        pool.enemies.update(1, kStep, pool.players, pool.generators.enemyObstacles(), nullptr, 1,
                            wrong);
    };
    Generators::LocalSnapshot generators;
    Enemies::LocalSnapshot enemies;
    s32 tick = 0;
    for (s32 window = 0; window < 64; ++window) {
        CAPTURE(window, tick, settled);
        pool.generators.saveLocal(generators);
        REQUIRE(pool.enemies.saveLocal(enemies));
        const s32 distance = window % 8 + 1;
        std::vector<decltype(observe())> expected;
        std::vector<Generators::LocalSnapshot> nativeExpected(static_cast<usize>(distance));
        for (s32 i = 0; i < distance; ++i) {
            step(tick + i, false);
            expected.push_back(observe());
            pool.generators.saveLocal(nativeExpected[static_cast<usize>(i)]);
        }
        REQUIRE(pool.generators.restoreLocal(generators));
        REQUIRE(pool.enemies.restoreLocal(enemies));
        for (s32 i = 0; i < distance; ++i) {
            step(tick + i, true);
        }
        REQUIRE(pool.generators.restoreLocal(generators));
        REQUIRE(pool.enemies.restoreLocal(enemies));
        for (s32 i = 0; i < distance; ++i) {
            step(tick + i, false);
            CHECK((observe() == expected[static_cast<usize>(i)]));
            Generators::LocalSnapshot corrected;
            pool.generators.saveLocal(corrected);
            CHECK(pool.generators.equalLocal(corrected, nativeExpected[static_cast<usize>(i)]));
        }
        tick += distance;
    }
    CHECK(pool.generators.count() > 3);
    CHECK(pool.generators.bodyShown(3));
    CHECK(pool.device.textureUpdates == 0);
}

TEST_CASE("native generator history cannot outlive its level or owner",
          "[local-rewind][generator-local-rewind][native-equality]") {
    Generators::LocalSnapshot saved;
    Generators a;
    Generators b;
    CHECK_FALSE(a.restoreLocal(saved));
    CHECK_FALSE(a.equalLocal(saved, saved));
    a.saveLocal(saved);
    CHECK_FALSE(b.restoreLocal(saved));
    CHECK_FALSE(b.equalLocal(saved, saved));
    REQUIRE(a.restoreLocal(saved));
    CHECK(a.equalLocal(saved, saved));
    a.clear();
    CHECK_FALSE(a.restoreLocal(saved));
    CHECK_FALSE(a.equalLocal(saved, saved));
    {
        const Generators temporary;
        temporary.saveLocal(saved);
    }
    CHECK_FALSE(a.restoreLocal(saved));
}

TEST_CASE("generator source lifetimes survive authored restore and corrected dynamic births",
          "[local-rewind][generator-local-rewind][native-sound-identity][assets]") {
    Pool pool;
    std::vector<u64> authored;
    for (usize index = 0; index < pool.generators.count(); ++index) {
        const auto instance = pool.generators.instanceOf(static_cast<s32>(index));
        REQUIRE(instance != 0);
        CHECK(std::ranges::find(authored, instance) == authored.end());
        authored.push_back(instance);
    }
    CHECK(pool.generators.instanceOf(-1) == 0);
    CHECK(pool.generators.instanceOf(static_cast<s32>(authored.size())) == 0);
    const auto diagnostic = pool.generators.checkpoint();
    REQUIRE(diagnostic);
    const auto hit = pool.generators.strike(0, 1, 0);
    REQUIRE(hit);
    CHECK(hit->instance == authored[0]);
    REQUIRE(pool.generators.restore(*diagnostic));
    for (usize index = 0; index < authored.size(); ++index) {
        CHECK(pool.generators.instanceOf(static_cast<s32>(index)) == authored[index]);
    }

    test::ProjectileCheckpointFixture art;
    ItemInfo info;
    info.type = ItemInfo::kGenerator;
    info.name = "BOSSGEN";
    info.hitPoints = 20;
    info.radius = 2;
    info.height = 5;
    const auto birth = [&](s32 kind) {
        return pool.generators.placeBoss(pool.device, info, art.archive, pool.enemies, kind,
                                         Mat4{1}, nullptr, "FLIGHT", true);
    };
    Generators::LocalSnapshot saved;
    pool.generators.saveLocal(saved);
    REQUIRE(birth(kGruntKind));
    const auto dynamic = static_cast<s32>(authored.size());
    const u64 first = pool.generators.instanceOf(dynamic);
    CHECK(first > authored.back());
    const auto destroyed = pool.generators.strike(dynamic, 1000, 0);
    REQUIRE(destroyed);
    REQUIRE(destroyed->destroyed);
    CHECK(destroyed->instance == first);
    CHECK(pool.generators.instanceOf(dynamic) == first);
    REQUIRE(birth(kGruntKind));
    const u64 second = pool.generators.instanceOf(dynamic + 1);
    CHECK(second > first);

    REQUIRE(pool.generators.restoreLocal(saved));
    CHECK(pool.generators.instanceOf(dynamic) == 0);
    // Rejected births do not consume a logical lifetime. Replaying accepted
    // births restores the original sequence, including a now-retired emitter.
    CHECK_FALSE(birth(-1));
    REQUIRE(birth(kGruntKind));
    CHECK(pool.generators.instanceOf(dynamic) == first);
    const auto replayed = pool.generators.strike(dynamic, 1000, 0);
    REQUIRE(replayed);
    CHECK(*replayed == *destroyed);
    REQUIRE(birth(kGruntKind));
    CHECK(pool.generators.instanceOf(dynamic + 1) == second);
}

TEST_CASE("native generator equality includes player admission and offscreen attention",
          "[local-rewind][generator-local-rewind][native-equality]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    Generators generators;
    REQUIRE(generators.bind(device, WorldLayout{}, enemies, nullptr, {}, 1));
    Generators::LocalSnapshot initial;
    generators.saveLocal(initial);
    const auto changed = [&](auto&& change) {
        REQUIRE(generators.restoreLocal(initial));
        change();
        Generators::LocalSnapshot after;
        generators.saveLocal(after);
        CHECK_FALSE(generators.equalLocal(initial, after));
        CHECK_FALSE(generators.equalLocal(after, initial));
        REQUIRE(generators.restoreLocal(initial));
        generators.saveLocal(after);
        CHECK(generators.equalLocal(initial, after));
    };
    changed([&] { generators.setPlayerCount(4); });
    changed([&] { generators.setView(ViewVolume{}); });
    changed([&] { generators.setView(std::nullopt, Vec3{7, 8, 9}); });
}

TEST_CASE("empty bound generator pools have resumable clocks but unbound pools do not",
          "[rollback][generators][generators-checkpoint]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    Generators generators;
    CHECK_FALSE(generators.checkpoint());
    REQUIRE(generators.bind(device, WorldLayout{}, enemies, nullptr, {}, 1));
    const auto empty = generators.checkpoint();
    REQUIRE(empty);
    CHECK(empty->valid());
    CHECK(empty->generators.empty());
    CHECK(empty->bodies.empty());
    REQUIRE(generators.restore(*empty));
    auto invalid = *empty;
    invalid.generators.resize(Generators::Checkpoint::kMaxGenerators + 1);
    CHECK_FALSE(invalid.valid());
    CHECK_FALSE(generators.restore(invalid));
    CHECK(generators.checkpoint() == empty);
    generators.clear();
    CHECK_FALSE(generators.checkpoint());
    CHECK_FALSE(generators.restore(*empty));
}

TEST_CASE("generator restoration preserves pending detached quota until the native next phase",
          "[rollback][generators][generators-checkpoint][assets]") {
    Pool pool;
    pool.generators.update(1, pool.enemies, pool.players);
    REQUIRE(pool.generators.livingOf(0) == 1);
    EnemyHit hit;
    hit.damage = 1000;
    hit.player = 0;
    REQUIRE(pool.enemies.hurt(0, hit));
    const auto generators = pool.generators.checkpoint();
    const auto enemies = pool.enemies.checkpoint();
    REQUIRE(generators);
    REQUIRE(enemies);
    REQUIRE_FALSE(enemies->generatorEvents.empty());
    const s32 wait = pool.generators.countdownOf(0);
    pool.generators.update(1, pool.enemies, pool.players);
    CHECK(pool.generators.livingOf(0) == 0);
    CHECK(pool.generators.countdownOf(0) == wait - 1);
    const auto expected = pool.generators.checkpoint();
    const auto expectedEnemies = pool.enemies.checkpoint();
    REQUIRE(pool.enemies.restore(*enemies));
    REQUIRE(pool.generators.restore(*generators));
    CHECK(pool.generators.livingOf(0) == 1); // do not reconstruct this from living bodies
    CHECK(pool.enemies.checkpoint()->generatorEvents == enemies->generatorEvents);
    pool.generators.update(1, pool.enemies, pool.players);
    CHECK(pool.generators.checkpoint() == expected);
    CHECK(pool.enemies.checkpoint() == expectedEnemies);
}

TEST_CASE("generator prepared restoration is atomic owner bound and invalidated by rebinding",
          "[rollback][generators][generators-checkpoint][assets]") {
    Pool pool;
    Pool other;
    const auto baseline = pool.generators.checkpoint();
    REQUIRE(baseline);
    pool.step(0);
    const auto current = pool.generators.checkpoint();
    const auto otherCurrent = other.generators.checkpoint();
    REQUIRE(current);
    CHECK(current != baseline);
    const auto uploads = pool.device.texturesCreated;
    auto prepared = pool.generators.prepareRestore(*baseline);
    auto sibling = pool.generators.prepareRestore(*baseline);
    REQUIRE(prepared);
    REQUIRE(sibling);
    CHECK(pool.generators.checkpoint() == current);
    CHECK(pool.generators.canCommit(*prepared));
    CHECK_FALSE(other.generators.canCommit(*prepared));
    CHECK_FALSE(other.generators.commitRestore(std::move(*prepared)));
    CHECK(other.generators.checkpoint() == otherCurrent);
    REQUIRE(pool.generators.commitRestore(std::move(*prepared)));
    CHECK(pool.generators.checkpoint() == baseline);
    CHECK_FALSE(pool.generators.canCommit(*prepared));
    CHECK_FALSE(pool.generators.commitRestore(std::move(*prepared)));
    CHECK_FALSE(pool.generators.canCommit(*sibling));
    CHECK_FALSE(pool.generators.commitRestore(std::move(*sibling)));
    CHECK(pool.device.texturesCreated == uploads);

    auto stale = pool.generators.prepareRestore(*baseline);
    REQUIRE(stale);
    REQUIRE(pool.generators.bind(pool.device, pool.layout, pool.enemies, nullptr,
                                 {1.5f, 1.25f, 1.5f}, 1));
    CHECK_FALSE(pool.generators.canCommit(*stale));
    CHECK_FALSE(pool.generators.commitRestore(std::move(*stale)));
    CHECK(pool.generators.checkpoint() == baseline);
}

TEST_CASE("prepared generators cannot cross an owner lifetime even at the same address",
          "[rollback][generators][generators-checkpoint]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    // Destroy/recreate inside the same optional uses precisely the same storage.
    std::optional<Generators> owner;
    owner.emplace();
    REQUIRE(owner->bind(device, WorldLayout{}, enemies, nullptr, {}, 1));
    const auto checkpoint = owner->checkpoint();
    REQUIRE(checkpoint);
    auto prepared = owner->prepareRestore(*checkpoint);
    REQUIRE(prepared);
    const auto* address = std::addressof(*owner);
    owner.reset();
    owner.emplace();
    REQUIRE(std::addressof(*owner) == address);
    REQUIRE(owner->bind(device, WorldLayout{}, enemies, nullptr, {}, 1));
    CHECK_FALSE(owner->canCommit(*prepared));
    CHECK_FALSE(owner->commitRestore(std::move(*prepared)));
    CHECK(owner->checkpoint() == checkpoint);
}

TEST_CASE("generator checkpoints reject malformed values different rosters and boss figures",
          "[rollback][generators][generators-checkpoint][assets]") {
    Pool pool;
    pool.step(0);
    const auto before = pool.generators.checkpoint();
    REQUIRE(before);
    const auto uploads = pool.device.texturesCreated;
    const auto reject = [&](const Generators::Checkpoint& state) {
        CHECK_FALSE(pool.generators.prepareRestore(state));
        CHECK_FALSE(pool.generators.restore(state));
        CHECK(pool.generators.checkpoint() == before);
        CHECK(pool.device.texturesCreated == uploads);
    };
    auto invalid = *before;
    invalid.generators[0].position.x = std::numeric_limits<f32>::quiet_NaN();
    reject(invalid);
    invalid = *before;
    invalid.generators[0].presence = static_cast<Generators::Slot::Presence>(255);
    reject(invalid);
    invalid = *before;
    invalid.generators[0].ratio = 1.01f;
    reject(invalid);
    invalid = *before;
    invalid.generators[0].living = -1;
    reject(invalid);
    invalid = *before;
    invalid.generators[0].box.enemyItem->radius = -1;
    reject(invalid);
    invalid = *before;
    invalid.generators[0].support = 999;
    reject(invalid);
    invalid = *before;
    invalid.bodies[0].textures.remainder = std::numeric_limits<f32>::infinity();
    reject(invalid);
    invalid = *before;
    invalid.bodies.push_back(invalid.bodies.front());
    reject(invalid);
    invalid = *before;
    invalid.roster[0].position.x += 1; // same count/kinds, a different trusted placement
    REQUIRE(invalid.valid());
    reject(invalid);
    invalid = *before;
    invalid.generators[0].threshold += 1;
    reject(invalid);
    invalid = *before;
    invalid.generators[0].boss = true;
    reject(invalid);
    invalid = *before;
    invalid.scales.rate = std::numeric_limits<f32>::quiet_NaN();
    reject(invalid);
    invalid = *before;
    invalid.players = 5;
    reject(invalid);

    auto prepared = pool.generators.prepareRestore(*before);
    REQUIRE(prepared);
    ItemInfo info;
    info.type = ItemInfo::kGenerator;
    info.name = "BOSSGEN";
    info.hitPoints = 10;
    ItemArchive empty;
    REQUIRE(pool.generators.placeBoss(pool.device, info, empty, pool.enemies, kGruntKind, Mat4{1},
                                      nullptr));
    CHECK_FALSE(pool.generators.checkpoint());
    CHECK_FALSE(pool.generators.prepareRestore(*before));
    CHECK_FALSE(pool.generators.canCommit(*prepared));
    CHECK_FALSE(pool.generators.commitRestore(std::move(*prepared)));
    pool.generators.clear();
    CHECK_FALSE(pool.generators.checkpoint());
    CHECK_FALSE(pool.generators.restore(*before));
}

TEST_CASE("special generator shared species cursor rewinds even when a birth was refused",
          "[rollback][generators][generators-checkpoint][assets]") {
    const s32 realm = GENERATE(5, 6);
    const auto root = nativeRoot();
    const std::string archive = realm == 5 ? "ITEMS/LEVELE" : "ITEMS/LEVELF";
    test::assetOrSkip(archive + "/ANIM.PS2");
    ItemArchive items;
    REQUIRE(items.load(root / archive));
    auto layout = layoutOf("generator-checkpoint-special", R"({
      "objects":[{"name":"GROUND","position":[0,0,0]}],
      "itemInfos":[{"type":3,"name":"SPECIAL","radius":2,"height":5,"hitPoints":10}],
      "itemInstances":[
        {"info":0,"position":[0,0,0],"params":[2,0,7,0,4,0,1,0,0,0,0,0]},
        {"info":0,"position":[30,0,0],"params":[2,0,7,0,4,0,1,0,0,0,0,0]}]})");
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 16, {}, 859);
    Generators generators;
    REQUIRE(generators.bind(device, layout, enemies, nullptr, {}, 1, {}, realm, &items));
    REQUIRE(generators.count() == 2);
    const std::array players{EnemyView{.player = 0, .position = {0, 0, 30}}};
    const std::array blockers{Obstacle{.halfAcross = 1000, .halfAlong = 1000, .height = 100}};
    generators.update(1, enemies, players, blockers);
    REQUIRE(generators.bredOf(0) == 0);
    REQUIRE(generators.bredOf(1) == 0);
    const auto initial = generators.checkpoint();
    const auto initialEnemies = enemies.checkpoint();
    REQUIRE(initial);
    REQUIRE(initialEnemies);
    CHECK(initial->specialBirth == 2);
    std::vector<Generators::Checkpoint> states;
    std::vector<Enemies::Checkpoint> opponents;
    for (s32 pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            REQUIRE(enemies.restore(*initialEnemies));
            REQUIRE(generators.restore(*initial));
        }
        for (s32 tick = 0; tick < 90; ++tick) {
            generators.update(1, enemies, players);
            enemies.update(1, kStep, players, generators.enemyObstacles());
            const auto state = generators.checkpoint();
            const auto enemyState = enemies.checkpoint();
            REQUIRE(state);
            REQUIRE(enemyState);
            if (pass == 0) {
                states.push_back(*state);
                opponents.push_back(*enemyState);
            } else {
                CAPTURE(realm, tick);
                CHECK(*state == states[static_cast<usize>(tick)]);
                CHECK(*enemyState == opponents[static_cast<usize>(tick)]);
            }
        }
    }
    CHECK(states.back().specialBirth > initial->specialBirth);
    CHECK(generators.bredOf(0) > 0);
    CHECK(generators.bredOf(1) > 0);
}

TEST_CASE("generator support correction restores exact local placement without touching the world",
          "[rollback][generators][generators-checkpoint][assets]") {
    auto layout = layoutOf("generator-checkpoint-support", R"({
      "objects":[{"name":"LIFT","position":[10,5,20],"flags":4102}],
      "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,
                    "xSize":3,"zSize":1,"hitPoints":10}],
      "itemInstances":[{"info":0,"position":[12,5,23],
                        "params":[1,0,7,0,5,0,20,0,0,0,0,0]}]})");
    WorldCollision collision;
    collision.build(
        {CollisionTriangle{.vertices = {Vec3{-30, 0, -30}, Vec3{30, 0, 30}, Vec3{30, 0, -30}},
                           .object = 0},
         CollisionTriangle{.vertices = {Vec3{-30, 0, -30}, Vec3{-30, 0, 30}, Vec3{30, 0, 30}},
                           .object = 0}});
    collision.setMovingObjects(std::array<s32, 1>{0});
    const Mat4 original = glm::translate(Mat4{1}, Vec3{10, -15, 20});
    collision.setObjectTransform(0, original);
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, nativeRoot(), &collision, 4, {}, 1);
    Generators generators;
    REQUIRE(generators.bind(device, layout, enemies, &collision, {}, 1));
    const auto before = generators.checkpoint();
    REQUIRE(before);
    REQUIRE(before->generators[0].support == 0);
    const Mat4 moved =
        glm::rotate(glm::translate(Mat4{1}, Vec3{30, 10, 40}), 1.57079637f, Vec3{0, 1, 0});
    collision.setObjectTransform(0, moved);
    generators.syncFloors();
    const auto after = generators.checkpoint();
    REQUIRE(after);
    CHECK(after != before);
    REQUIRE(generators.restore(*before));
    CHECK(collision.objectTransform(0) == moved);
    CHECK(generators.checkpoint() == before); // restore does not re-run syncFloors
    generators.syncFloors();
    CHECK(generators.checkpoint() == after);
    collision.setObjectTransform(0, original);
    REQUIRE(generators.restore(*before));
    generators.syncFloors();
    CHECK(generators.checkpoint() == before);

    auto staged = generators.prepareRestore(*before);
    REQUIRE(staged);
    collision.clear();
    CHECK_FALSE(generators.canCommit(*staged));
    CHECK_FALSE(generators.commitRestore(std::move(*staged)));
    CHECK(generators.checkpoint() == before);
}
} // namespace
