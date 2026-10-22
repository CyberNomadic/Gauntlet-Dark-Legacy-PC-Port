#include <array>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/critters/CrittersCheckpoint.h"
#include "game/rollback/LocalStateHistory.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct NativePopulation {
    test::FakeRenderDevice device;
    Critters critters;
    NativePopulation() {
        const auto root = test::assetOrSkip("CRITTER/GOLEM.WAD").parent_path().parent_path();
        critters.open(device, root, nullptr, {}, 'G');
        critters.setLookouts({{{-20, 0, 0}, {-20, 0, 20}}, {1, 65535}});
    }
    CrittersCheckpoint::State observe() {
        const CrittersCheckpoint observation{critters};
        auto value = observation.checkpoint();
        REQUIRE(value);
        return std::move(*value);
    }
    void step(s32 tick, bool incorrect = false) {
        if (tick % 24 == 0) {
            const s32 kind = (tick / 24 + (incorrect ? 1 : 0)) % 3;
            const Vec3 position{static_cast<f32>(tick % 19 - 9), 0, static_cast<f32>(tick % 23)};
            if (kind == 0) {
                critters.spawnGeneral(position, 0, 20);
            }
            if (kind == 1) {
                critters.spawnGolem(position, 0);
            }
            if (kind == 2) {
                critters.spawnGargoyle(position, 0);
            }
        }
        if (tick % 17 == 0) {
            EnemyHit hit;
            hit.player = 3;
            hit.damage = incorrect ? 1.0f : 100000.0f;
            hit.level = 99;
            critters.hurt((tick / 17) % Critters::kMost, hit);
        }
        if (tick % 37 == 0) {
            critters.freeze(0, 5);
        }
        if (tick % 43 == 0) {
            critters.blind(1, 7);
        }
        std::array<EnemyView, 4> players;
        for (s32 seat = 0; seat < 4; ++seat) {
            auto& player = players[static_cast<usize>(seat)];
            player.player = seat;
            player.radius = 1;
            player.height = 6;
            player.position = {static_cast<f32>(seat * 4 - 7), 0,
                               static_cast<f32>(tick % 21 + (incorrect ? 30 : 3))};
        }
        critters.update(1, 1.0f / 60, players, tick % 61 < 5);
        if (tick % 13 == 0) {
            critters.takeBlows();
            critters.takeCues();
            critters.takeShots();
            critters.takeLosses();
        }
    }
};

TEST_CASE("native great-creature population rewinds spawns queues patrols and bank clocks",
          "[local-rewind][critters-local-rewind][native-equality][assets]") {
    auto reference = std::make_unique<NativePopulation>();
    auto predicted = std::make_unique<NativePopulation>();
    LocalStateHistory<Critters::LocalSnapshot> history;
    s32 tick = 0;
    for (s32 window = 0; window < 96; ++window) {
        CAPTURE(window, tick);
        const s32 distance = window % 8 + 1;
        REQUIRE(history.capture(static_cast<u64>(tick),
                                [&](auto& state) { return predicted->critters.saveLocal(state); }));
        const auto* saved = history.find(static_cast<u64>(tick));
        REQUIRE(saved);
        std::vector<Critters::LocalSnapshot> nativeExpected(static_cast<usize>(distance));
        for (s32 i = 0; i < distance; ++i) {
            predicted->step(tick + i);
            REQUIRE(predicted->critters.saveLocal(nativeExpected[static_cast<usize>(i)]));
        }
        REQUIRE(predicted->critters.restoreLocal(*saved));
        for (s32 i = 0; i < distance; ++i) {
            predicted->step(tick + i, true);
        }
        REQUIRE(predicted->critters.restoreLocal(*saved));
        CHECK(predicted->observe() == reference->observe());
        for (s32 i = 0; i < distance; ++i) {
            CAPTURE(i);
            reference->step(tick + i);
            predicted->step(tick + i);
            CHECK(predicted->observe() == reference->observe());
            auto corrected = std::make_unique<Critters::LocalSnapshot>();
            REQUIRE(predicted->critters.saveLocal(*corrected));
            CHECK(
                predicted->critters.equalLocal(*corrected, nativeExpected[static_cast<usize>(i)]));
        }
        tick += distance;
    }
    CHECK(predicted->device.textureUpdates == 0);
}

TEST_CASE("native population retains speculative artwork without retaining its active catalog",
          "[local-rewind][critters-local-rewind][native-equality][assets]") {
    auto population = std::make_unique<NativePopulation>();
    auto& critters = population->critters;
    Critters::LocalSnapshot cold;
    Critters::LocalSnapshot general;
    REQUIRE(critters.saveLocal(cold));
    const auto empty = population->observe();
    REQUIRE(critters.spawnGeneral({}, 0, 15) == 0);
    REQUIRE(critters.saveLocal(general));
    const auto baseline = population->observe();
    const auto* artwork = critters.resources().front();
    const auto* actor = &critters.fighters().front();
    REQUIRE(critters.spawnGolem({4, 0, 3}, 0) == 1);
    REQUIRE(critters.spawnGargoyle({8, 0, 3}, 0) == 2);
    const auto uploads = population->device.texturesCreated;
    REQUIRE(critters.restoreLocal(cold));
    CHECK(population->observe() == empty);
    CHECK(critters.resources().empty());
    REQUIRE(critters.spawnGeneral({}, 0, 15) == 0);
    CHECK(critters.resources().front() == artwork);
    CHECK(&critters.fighters().front() == actor);
    CHECK(population->observe() == baseline);
    REQUIRE(critters.spawnGolem({4, 0, 3}, 0) == 1);
    REQUIRE(critters.spawnGargoyle({8, 0, 3}, 0) == 2);
    REQUIRE(critters.restoreLocal(general));
    CHECK(population->observe() == baseline);
    CHECK(population->device.texturesCreated == uploads);
    critters.setLookouts({{{20, 0, 0}, {20, 0, 20}}, {1, 65535}});
    REQUIRE(critters.restoreLocal(general));
    CHECK(population->observe() == baseline);
    auto corrected = std::make_unique<Critters::LocalSnapshot>();
    REQUIRE(critters.saveLocal(*corrected));
    CHECK(critters.equalLocal(general, *corrected));
}

TEST_CASE("native population snapshots cannot cross ownership or stage unload",
          "[local-rewind][critters-local-rewind][native-equality]") {
    auto first = std::make_unique<Critters>();
    auto other = std::make_unique<Critters>();
    auto saved = std::make_unique<Critters::LocalSnapshot>();
    CHECK_FALSE(first->equalLocal(*saved, *saved));
    REQUIRE(first->saveLocal(*saved));
    CHECK(first->equalLocal(*saved, *saved));
    CHECK_FALSE(other->restoreLocal(*saved));
    CHECK_FALSE(other->equalLocal(*saved, *saved));
    REQUIRE(first->restoreLocal(*saved));
    first->close();
    CHECK_FALSE(first->restoreLocal(*saved));
    CHECK_FALSE(first->equalLocal(*saved, *saved));
    REQUIRE(first->saveLocal(*saved));
    REQUIRE(first->restoreLocal(*saved));
}

TEST_CASE("native empty population equality still tracks its patrol route",
          "[local-rewind][critters-local-rewind][native-equality]") {
    auto population = std::make_unique<Critters>();
    auto initial = std::make_unique<Critters::LocalSnapshot>();
    auto after = std::make_unique<Critters::LocalSnapshot>();
    REQUIRE(population->saveLocal(*initial));
    population->setLookouts({{{1, 2, 3}, {4, 5, 6}}, {1, 65535}});
    REQUIRE(population->saveLocal(*after));
    CHECK_FALSE(population->equalLocal(*initial, *after));
    REQUIRE(population->restoreLocal(*initial));
    REQUIRE(population->saveLocal(*after));
    CHECK(population->equalLocal(*initial, *after));
}
} // namespace
