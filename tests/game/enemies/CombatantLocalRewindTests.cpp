#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/BossDefinition.h"
#include "game/enemies/CombatantCheckpoint.h"
#include "game/enemies/Gargoyle.h"
#include "game/enemies/General.h"
#include "game/enemies/Golem.h"
#include "game/netplay/LocalStateHistory.h"
#include "game/netplay/NativeProbeTiming.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr f32 kStep = 1.0f / 60;

CombatantDefinition familyOf(std::string_view name) {
    if (name == "GENERAL") {
        return General::definition();
    }
    if (name == "GOLEM") {
        return Golem::definition();
    }
    if (name == "GARG") {
        return Gargoyle::definition();
    }
    return bossDefinition(name);
}

struct Encounter {
    test::FakeRenderDevice device;
    CombatantAssets stock;
    Combatant reference, predicted;
    std::unique_ptr<CombatantCheckpoint> referenceObservation, predictedObservation;

    explicit Encounter(std::string_view name) {
        const auto root = test::assetOrSkip("CRITTER/GOLEM.WAD").parent_path().parent_path();
        REQUIRE(stock.load(device, root, familyOf(name), 'G'));
        for (auto* actor : {&reference, &predicted}) {
            REQUIRE(actor->spawn(stock, 40, {}, 0, nullptr, {}, 'G'));
        }
        // These diagnostic adapters only observe values for comparison. Rewind
        // never serializes, prepares, commits or rebinds a portable checkpoint.
        const std::array resources{CombatantCheckpoint::Resource{1, &stock}};
        referenceObservation = std::make_unique<CombatantCheckpoint>(
            reference, resources, CombatantCheckpoint::Bindings{});
        predictedObservation = std::make_unique<CombatantCheckpoint>(
            predicted, resources, CombatantCheckpoint::Bindings{});
    }

    void step(Combatant& actor, s32 frame, bool wrong = false) {
        if (frame % 83 == 0) {
            REQUIRE(actor.spawn(stock, 40, {}, 0, nullptr, {}, 'G'));
            actor.setArenaTargets(
                std::array{CombatArenaTarget{2, glm::translate(Mat4{1}, Vec3{3, 0, 8}), false},
                           CombatArenaTarget{5, glm::translate(Mat4{1}, Vec3{-3, 0, 8}), true}});
            actor.setArenaAnchors(std::array{Mat4{1}});
        }
        if (frame % 83 == 71) {
            EnemyHit hit;
            hit.player = wrong ? 3 : 1;
            hit.level = 99;
            hit.damage = wrong ? 3.0f : actor.maxHealth() * 16;
            actor.hurt(hit);
        }
        if (frame % 31 == 5) {
            actor.freeze(wrong ? 2 : 4);
        }
        if (frame % 37 == 6) {
            actor.blind(wrong ? 3 : 7);
        }
        if (frame % 41 == 7) {
            actor.curb(wrong ? 0.1f : 0.2f);
        }
        actor.hold(frame % 53 < 3);
        actor.setShrink(frame % 47 < 8 ? 0.5f : 1.0f);
        actor.damagedPlayer(wrong ? 3 : 1, 0.5f);
        std::array<EnemyView, 4> players;
        for (s32 seat = 0; seat < 4; ++seat) {
            auto& view = players[static_cast<usize>(seat)];
            view.player = seat;
            view.radius = 1;
            view.height = 6;
            view.position = {static_cast<f32>(seat * 3 - 5), 0,
                             static_cast<f32>(frame % 11 + (wrong ? 25 : 6))};
        }
        actor.update(1, kStep, players, {}, frame % 59 < 5);
        if (frame % 13 == 0) {
            // Some outputs survive a tick boundary; others have already been
            // consumed. Both paths must be restored exactly on correction.
            actor.takeCues();
            actor.takeBlows();
            actor.takeShots();
            actor.takeLosses();
        }
    }

    void equal() const {
        const auto expected = referenceObservation->checkpoint();
        const auto actual = predictedObservation->checkpoint();
        REQUIRE(expected);
        REQUIRE(actual);
        CHECK(*actual == *expected);
    }
};

TEST_CASE("native combatant families replay corrected one to eight tick histories",
          "[local-rewind][combatant-local-rewind][native-equality][assets]") {
    const auto* const name =
        GENERATE("GENERAL", "GOLEM", "GARG", "CHIMERA", "LICH", "GARM", "YETI", "DRAGON", "DJINN",
                 "DRIDER", "PBOSS", "WRAITH", "SKORNE1", "SKORNE2");
    CAPTURE(name);
    auto encounter = std::make_unique<Encounter>(name);
    LocalStateHistory<Combatant::LocalSnapshot> history;
    s32 frame = 0;
    const u32 uploads = encounter->device.texturesCreated;
    for (s32 window = 0; window < 64; ++window) {
        const s32 distance = window % 8 + 1;
        REQUIRE(history.capture(static_cast<u64>(frame), [&](auto& saved) {
            return encounter->predicted.saveLocal(saved);
        }));
        const auto* saved = history.find(static_cast<u64>(frame));
        REQUIRE(saved);
        // The native comparison uses this exact owner and retained bindings;
        // the independent second actor remains a separate diagnostic oracle.
        std::vector<Combatant::LocalSnapshot> nativeExpected(static_cast<usize>(distance));
        for (s32 i = 0; i < distance; ++i) {
            encounter->step(encounter->predicted, frame + i);
            REQUIRE(encounter->predicted.saveLocal(nativeExpected[static_cast<usize>(i)]));
        }
        REQUIRE(encounter->predicted.restoreLocal(*saved));
        for (s32 i = 0; i < distance; ++i) {
            encounter->step(encounter->predicted, frame + i, true);
        }
        REQUIRE(encounter->predicted.restoreLocal(*saved));
        encounter->equal();
        for (s32 i = 0; i < distance; ++i) {
            CAPTURE(window, distance, frame, i);
            encounter->step(encounter->reference, frame + i);
            encounter->step(encounter->predicted, frame + i);
            encounter->equal();
            Combatant::LocalSnapshot corrected;
            REQUIRE(encounter->predicted.saveLocal(corrected));
            CHECK(
                encounter->predicted.equalLocal(corrected, nativeExpected[static_cast<usize>(i)]));
        }
        frame += distance;
    }
    CHECK(encounter->device.texturesCreated == uploads);
    CHECK(encounter->device.textureUpdates == 0);
}

TEST_CASE("local combatant rewind retains boss child addresses across clear and kind reuse",
          "[local-rewind][combatant-local-rewind][native-equality][assets]") {
    auto encounter = std::make_unique<Encounter>("CHIMERA");
    auto& actor = encounter->predicted;
    REQUIRE(actor.childCount() == 3);
    std::array<const Combatant*, 3> children{};
    for (s32 i = 0; i < 3; ++i) {
        children[static_cast<usize>(i)] = actor.child(41 + i);
    }
    Combatant::LocalSnapshot saved;
    REQUIRE(actor.saveLocal(saved));
    const auto original = encounter->predictedObservation->checkpoint();
    REQUIRE(original);
    const auto root = test::assetOrSkip("CRITTER/GOLEM.WAD").parent_path().parent_path();
    CombatantAssets general;
    REQUIRE(general.load(encounter->device, root, General::definition(), 'G'));
    const auto uploads = encounter->device.texturesCreated;
    for (s32 run = 0; run < 3; ++run) {
        actor.clear();
        CHECK(actor.childCount() == 0);
        CHECK(actor.child(41) == nullptr);
        REQUIRE(actor.spawn(general, 80, {2, 0, 3}, 1, nullptr, {}, 'G'));
        REQUIRE(actor.restoreLocal(saved));
        CHECK(encounter->predictedObservation->checkpoint() == original);
        Combatant::LocalSnapshot corrected;
        REQUIRE(actor.saveLocal(corrected));
        CHECK(actor.equalLocal(saved, corrected));
        for (s32 i = 0; i < 3; ++i) {
            CHECK(actor.child(41 + i) == children[static_cast<usize>(i)]);
        }
    }
    CHECK(encounter->device.texturesCreated == uploads);
}

TEST_CASE("local combatant snapshots fence owners assets and borrowed per-tick contacts",
          "[local-rewind][combatant-local-rewind][native-equality][assets]") {
    auto encounter = std::make_unique<Encounter>("GOLEM");
    auto& actor = encounter->predicted;
    Combatant::LocalSnapshot saved;
    Combatant::LocalSnapshot empty;
    REQUIRE(actor.saveLocal(saved));
    CHECK(actor.equalLocal(saved, saved));
    CHECK_FALSE(actor.restoreLocal(empty));
    CHECK_FALSE(actor.equalLocal(empty, empty));
    CHECK_FALSE(encounter->reference.restoreLocal(saved));
    CHECK_FALSE(encounter->reference.equalLocal(saved, saved));
    const auto original = encounter->predictedObservation->checkpoint();
    REQUIRE(original);
    auto stalePortable = encounter->predictedObservation->prepareRestore(*original);
    REQUIRE(stalePortable);
    REQUIRE(actor.restoreLocal(saved));
    CHECK_FALSE(encounter->predictedObservation->canCommit(*stalePortable));
    std::array contacts{CombatantObstacle{}};
    actor.setObstacles(contacts);
    CHECK_FALSE(actor.restoreLocal(saved));
    CHECK_FALSE(actor.equalLocal(saved, saved));
    CHECK_FALSE(actor.saveLocal(empty));
    actor.setObstacles({});
    std::array bodies{EnemyBody{}};
    actor.setSwarm(bodies);
    CHECK_FALSE(actor.restoreLocal(saved));
    CHECK_FALSE(actor.equalLocal(saved, saved));
    CHECK_FALSE(actor.saveLocal(empty));
    actor.setSwarm({});
    REQUIRE(actor.restoreLocal(saved));
    CHECK(encounter->predictedObservation->checkpoint() == original);
    encounter->stock.clear();
    CHECK_FALSE(actor.canRestoreLocal(saved));
    CHECK_FALSE(actor.equalLocal(saved, saved));
    CHECK_FALSE(actor.restoreLocal(saved));
    actor.clear();
    encounter->reference.clear();
}

TEST_CASE("local combatant model replacements use retained resources after correction",
          "[local-rewind][combatant-local-rewind][native-equality][assets]") {
    auto encounter = std::make_unique<Encounter>("GOLEM");
    auto& actor = encounter->predicted;
    const auto found = std::ranges::find_if(encounter->stock.tree->nodes, [&](const auto& node) {
        return !node.object.empty() &&
               encounter->stock.archive.models.find(node.object).has_value();
    });
    REQUIRE(found != encounter->stock.tree->nodes.end());
    REQUIRE(actor.replaceNodeModel(encounter->device, found->name, found->object));
    Combatant::LocalSnapshot saved;
    REQUIRE(actor.saveLocal(saved));
    const auto expected = encounter->predictedObservation->checkpoint();
    const auto uploads = encounter->device.texturesCreated;
    actor.tint(Color{12, 98, 133, 255});
    REQUIRE(actor.replaceNodeModel(encounter->device, found->name, ""));
    REQUIRE(actor.restoreLocal(saved));
    CHECK(encounter->predictedObservation->checkpoint() == expected);
    actor.draw(encounter->device, Mat4{1}, WorldLighting{});
    CHECK(encounter->device.texturesCreated == uploads);
    CHECK(encounter->device.textureUpdates == 0);
    CHECK_FALSE(encounter->device.draws.empty());
    Combatant::LocalSnapshot rendered;
    REQUIRE(actor.saveLocal(rendered));
    CHECK(actor.equalLocal(saved, rendered)); // shared draw caches are not native state
}

TEST_CASE("local combatant rewind has no portable-packet scalar admission limits",
          "[local-rewind][combatant-local-rewind][native-equality][assets]") {
    auto encounter = std::make_unique<Encounter>("GOLEM");
    auto& actor = encounter->predicted;
    EnemyHit hit;
    hit.player = 2;
    hit.level = 99;
    hit.damage = 4000000;
    actor.hurt(hit);
    REQUIRE(actor.dying());
    const f32 health = actor.health();
    REQUIRE(health < -1000000);
    CHECK_FALSE(encounter->predictedObservation->checkpoint());
    Combatant::LocalSnapshot saved;
    REQUIRE(actor.saveLocal(saved));
    actor.clear();
    REQUIRE(actor.restoreLocal(saved));
    CHECK(actor.dying());
    CHECK(actor.health() == health);
    Combatant::LocalSnapshot after;
    REQUIRE(actor.saveLocal(after));
    CHECK(actor.equalLocal(saved, after));
    REQUIRE_FALSE(actor.takeLosses().empty());
    REQUIRE(actor.saveLocal(after));
    CHECK_FALSE(actor.equalLocal(saved, after));
    REQUIRE(actor.restoreLocal(saved));
    REQUIRE(actor.saveLocal(after));
    CHECK(actor.equalLocal(saved, after));
}

TEST_CASE("destroyed combatant owners cannot restore into reused addresses",
          "[local-rewind][combatant-local-rewind][native-equality]") {
    alignas(Combatant) std::array<std::byte, sizeof(Combatant)> storage{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): byte view of a trivially copyable buffer
    auto* actor = std::construct_at(reinterpret_cast<Combatant*>(storage.data()));
    Combatant::LocalSnapshot saved;
    REQUIRE(actor->saveLocal(saved));
    std::destroy_at(actor);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): byte view of a trivially copyable buffer
    actor = std::construct_at(reinterpret_cast<Combatant*>(storage.data()));
    CHECK_FALSE(actor->restoreLocal(saved));
    CHECK_FALSE(actor->equalLocal(saved, saved));
    std::destroy_at(actor);
}

TEST_CASE("native combatant equality observes hidden inactive values",
          "[local-rewind][combatant-local-rewind][native-equality]") {
    Combatant actor;
    Combatant::LocalSnapshot initial;
    REQUIRE(actor.saveLocal(initial));
    const auto changed = [&](auto&& change) {
        REQUIRE(actor.restoreLocal(initial));
        change();
        CHECK_FALSE(actor.present());
        Combatant::LocalSnapshot after;
        REQUIRE(actor.saveLocal(after));
        CHECK_FALSE(actor.equalLocal(initial, after));
        CHECK_FALSE(actor.equalLocal(after, initial));
        REQUIRE(actor.restoreLocal(initial));
        REQUIRE(actor.saveLocal(after));
        CHECK(actor.equalLocal(initial, after));
    };
    changed([&] { actor.takeFullHarm(true); });
    changed([&] { actor.tint(Color{21, 32, 43, 255}); });
    changed([&] { actor.setArenaAnchors(std::array{glm::translate(Mat4{1}, Vec3{3, 0, 4})}); });
    changed([&] {
        actor.setArenaTargets(
            std::array{CombatArenaTarget{2, Mat4{1}, false}, CombatArenaTarget{5, Mat4{1}, true}});
    });
}

TEST_CASE("native combatant equality retains a cleared slot's next incarnation",
          "[local-rewind][combatant-local-rewind][native-equality][assets]") {
    auto encounter = std::make_unique<Encounter>("GENERAL");
    auto& actor = encounter->predicted;
    actor.clear();
    Combatant::LocalSnapshot before;
    Combatant::LocalSnapshot after;
    REQUIRE(actor.saveLocal(before));
    const auto incarnation = actor.incarnation();
    REQUIRE(actor.spawn(encounter->stock, 40, {}, 0, nullptr, {}, 'G'));
    actor.clear();
    REQUIRE(actor.saveLocal(after));
    CHECK_FALSE(actor.present());
    CHECK(actor.incarnation() == incarnation + 1);
    CHECK_FALSE(actor.equalLocal(before, after));
    REQUIRE(actor.restoreLocal(before));
    REQUIRE(actor.saveLocal(after));
    CHECK(actor.equalLocal(before, after));
}

TEST_CASE("measure native multi-part combatant copy and eight tick replay",
          "[.][local-rewind-timing][combatant-local-rewind-timing][assets]") {
    auto encounter = std::make_unique<Encounter>("CHIMERA");
    LocalStateHistory<Combatant::LocalSnapshot> history;
    for (s32 i = 0; i < 60; ++i) {
        encounter->step(encounter->predicted, i);
    }
    const auto capture = [&](auto& value) { return encounter->predicted.saveLocal(value); };
    for (u64 i = 0;
         i < gdl::game::LocalStateHistory<gdl::game::Combatant::LocalSnapshot>::kCapacity; ++i) {
        REQUIRE(history.capture(i, capture));
    }
    using Clock = std::chrono::steady_clock;
    std::vector<f64> saves;
    std::vector<f64> restores;
    std::vector<f64> replays;
    for (u64 sample = 16; sample < 272; ++sample) {
        const auto begin = Clock::now();
        const bool captured = history.capture(sample, capture);
        const auto end = Clock::now();
        REQUIRE(captured);
        const auto* saved = history.find(sample);
        REQUIRE(saved);
        for (s32 i = 60; i < 68; ++i) {
            encounter->step(encounter->predicted, i, true);
        }
        const auto restoreStart = Clock::now();
        const bool restored = encounter->predicted.restoreLocal(*saved);
        const auto restoreEnd = Clock::now();
        REQUIRE(restored);
        const auto replayStart = Clock::now();
        for (s32 i = 60; i < 68; ++i) {
            encounter->step(encounter->predicted, i);
        }
        const auto replayEnd = Clock::now();
        REQUIRE(encounter->predicted.restoreLocal(*saved));
        saves.push_back(std::chrono::duration<f64, std::milli>(end - begin).count());
        restores.push_back(
            std::chrono::duration<f64, std::milli>(restoreEnd - restoreStart).count());
        replays.push_back(std::chrono::duration<f64, std::milli>(replayEnd - replayStart).count());
    }
    const auto report = [](const char* operation, const std::vector<f64>& samples) {
        const auto summary = test::summarizeNativeTimings(samples);
        REQUIRE(summary);
        std::cout << "combatant-local-" << operation << " samples=" << summary->samples
                  << " mean_ms=" << summary->mean << " p99_ms=" << summary->p99
                  << " max_ms=" << summary->maximum << '\n';
    };
    report("save", saves);
    report("restore", restores);
    report("replay8", replays);
}
} // namespace
