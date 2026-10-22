#include <algorithm>
#include <array>
#include <limits>
#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/bosses/BossDefinition.h"
#include "game/critters/CombatantCheckpoint.h"
#include "game/critters/Gargoyle.h"
#include "game/critters/General.h"
#include "game/critters/Golem.h"

namespace {
using namespace gdl;
using namespace gdl::game;

EnemyView visiblePlayer(s32 id, Vec3 position) {
    EnemyView view;
    view.player = id;
    view.position = position;
    view.radius = 1;
    view.height = 6;
    return view;
}
CombatantCheckpoint::State capture(const CombatantCheckpoint& owner) {
    auto state = owner.checkpoint();
    REQUIRE(state);
    return std::move(*state);
}
struct Fighter {
    test::FakeRenderDevice device;
    CombatantAssets stock;
    Combatant actor;
    std::unique_ptr<CombatantCheckpoint> checkpoint;
    explicit Fighter(const CombatantDefinition& family) {
        const auto root = test::assetOrSkip("CRITTER/GOLEM.WAD").parent_path().parent_path();
        REQUIRE(stock.load(device, root, family, 'G'));
        REQUIRE(actor.spawn(stock, 40, {}, 0, nullptr, {}, 'G'));
        checkpoint = std::make_unique<CombatantCheckpoint>(
            actor, std::array{CombatantCheckpoint::Resource{1, &stock}},
            CombatantCheckpoint::Bindings{});
    }
};

TEST_CASE("combatant checkpoint refuses malformed values and never reads transient contacts",
          "[combatant-checkpoint]") {
    Combatant actor;
    CombatantCheckpoint owner(actor, {}, {});
    const auto baseline = capture(owner);
    REQUIRE(baseline.valid());
    const auto reject = [&](auto mutate) {
        auto bad = baseline;
        mutate(bad);
        CHECK_FALSE(owner.restore(bad));
        CHECK(capture(owner) == baseline);
    };
    reject([](auto& state) { state.parts.clear(); });
    reject([](auto& state) {
        state.parts.front().actor.health = std::numeric_limits<f32>::quiet_NaN();
    });
    reject([](auto& state) { state.parts.front().arenaRandom = 0; });
    reject([](auto& state) { state.parts.front().arenaRandom = 2147483647U; });
    reject([](auto& state) { state.parts.front().actor.definition = {99, 0}; });
    reject([](auto& state) { state.parts.front().actor.player.playing = true; });
    reject([](auto& state) {
        state.parts.front().actor.patternTimes = {std::numeric_limits<f32>::infinity()};
    });
    reject([](auto& state) {
        state.parts.front().events.cues.resize(CombatantCheckpoint::kMaxEvents + 1);
    });
    const std::array scratch{CombatantObstacle{}};
    actor.setObstacles(scratch);
    CHECK_FALSE(owner.checkpoint());
    CHECK_FALSE(owner.prepareRestore(baseline));
    actor.setObstacles({});
    const std::array swarm{EnemyBody{}};
    actor.setSwarm(swarm);
    CHECK_FALSE(owner.checkpoint());
    actor.setSwarm({});
    CHECK(capture(owner) == baseline);
    actor.update(1, 1.0f / 60, {});
    const auto emptyAdvanced = capture(owner);
    REQUIRE(emptyAdvanced.parts.front().actor.smoothValid);
    REQUIRE(owner.restore(emptyAdvanced));
    CHECK(capture(owner) == emptyAdvanced);
}

TEST_CASE("native retired combatant slots preserve identity and may be restored before reuse",
          "[combatant-checkpoint][assets]") {
    Fighter native(General::definition());
    Fighter replay(General::definition());
    const auto alive = capture(*native.checkpoint);
    EnemyHit hit;
    hit.player = 2;
    hit.damage = 1000000;
    native.actor.hurt(hit);
    for (s32 tick = 0; tick < 1200 && native.actor.present(); ++tick) {
        native.actor.update(2, 1.0f / 30, {});
    }
    REQUIRE_FALSE(native.actor.present());
    const auto retired = capture(*native.checkpoint);
    REQUIRE(retired.parts.size() == 1);
    CHECK(retired.parts[0].actor.definition.resource == 0);
    CHECK(retired.parts[0].id == alive.parts[0].id);
    CHECK(retired.parts[0].incarnation == alive.parts[0].incarnation);
    REQUIRE_FALSE(retired.parts[0].events.losses.empty());
    const auto uploads = replay.device.texturesCreated;
    REQUIRE(replay.checkpoint->restore(retired));
    CHECK(capture(*replay.checkpoint) == retired);
    auto bad = retired;
    bad.parts[0].actor.player.playing = true;
    CHECK_FALSE(replay.checkpoint->restore(bad));
    bad = retired;
    bad.parts[0].actor.state = 1;
    CHECK_FALSE(replay.checkpoint->restore(bad));
    CHECK(capture(*replay.checkpoint) == retired);
    REQUIRE(native.actor.spawn(native.stock, 40, {2, 0, 3}, 0, nullptr, {}, 'G'));
    REQUIRE(replay.actor.spawn(replay.stock, 40, {2, 0, 3}, 0, nullptr, {}, 'G'));
    const auto reused = capture(*native.checkpoint);
    CHECK(reused.parts[0].incarnation > retired.parts[0].incarnation);
    CHECK(capture(*replay.checkpoint) == reused);
    REQUIRE(replay.checkpoint->restore(retired));
    REQUIRE(replay.checkpoint->restore(reused));
    CHECK(capture(*replay.checkpoint) == reused);
    CHECK(replay.device.texturesCreated == uploads);
}

TEST_CASE("combatant prepared values are owner bound single use and cleared on native teardown",
          "[combatant-checkpoint]") {
    Combatant actor;
    Combatant other;
    CombatantCheckpoint owner(actor, {}, {});
    const CombatantCheckpoint foreign(other, {}, {});
    const auto baseline = capture(owner);
    auto first = owner.prepareRestore(baseline);
    auto sibling = owner.prepareRestore(baseline);
    REQUIRE(first);
    REQUIRE(sibling);
    CHECK_FALSE(foreign.canCommit(*first));
    REQUIRE(owner.commitRestore(std::move(*first)));
    CHECK_FALSE(owner.canCommit(*first));
    CHECK_FALSE(owner.canCommit(*sibling));
    auto stale = owner.prepareRestore(baseline);
    REQUIRE(stale);
    actor.clear();
    CHECK_FALSE(owner.canCommit(*stale));
    CHECK(capture(owner) == baseline);
}

TEST_CASE(
    "native great creature continuation restores animation contacts damage and ordered queues",
    "[combatant-checkpoint][assets]") {
    const s32 rate = GENERATE(30, 60);
    const s32 family = GENERATE(0, 1, 2);
    const std::array definitions{Golem::definition(), General::definition(),
                                 Gargoyle::definition()};
    CAPTURE(rate, family);
    auto first = std::make_unique<Fighter>(definitions[static_cast<usize>(family)]);
    auto replay = std::make_unique<Fighter>(definitions[static_cast<usize>(family)]);
    auto views = std::array{visiblePlayer(3, {1, 0, 8}), visiblePlayer(1, {-8, 0, 11})};
    for (s32 frame = 0; frame < 25; ++frame) {
        first->actor.update(60 / rate, 1.0f / static_cast<f32>(rate), views);
    }
    first->actor.damagedPlayer(3, 7);
    first->actor.freeze(3);
    first->actor.blind(7);
    first->actor.curb(0.2f);
    first->actor.setShrink(0.75f);
    first->actor.tint(Color{31, 82, 193, 201});
    EnemyHit hit;
    hit.player = 1;
    hit.damage = 13;
    hit.level = 20;
    hit.flags = 0x10;
    hit.close = true;
    first->actor.hurt(hit);
    const auto baseline = capture(*first->checkpoint);
    REQUIRE(replay->checkpoint->restore(baseline));
    const u32 textures = replay->device.texturesCreated;
    CHECK(capture(*replay->checkpoint) == baseline);
    bool changed = false;
    for (s32 frame = 0; frame < 150; ++frame) {
        CAPTURE(frame);
        views[0].position.x = frame < 60 ? 1.0f : -3.0f;
        views[1].hidden = frame > 90;
        first->actor.update(60 / rate, 1.0f / static_cast<f32>(rate), views, {},
                            frame >= 40 && frame < 50);
        replay->actor.update(60 / rate, 1.0f / static_cast<f32>(rate), views, {},
                             frame >= 40 && frame < 50);
        const auto expected = capture(*first->checkpoint);
        CHECK(capture(*replay->checkpoint) == expected);
        changed = changed || expected != baseline;
        if (frame == 73) {
            auto undo = replay->checkpoint->prepareRestore(expected);
            REQUIRE(undo);
            replay->actor.hurt(hit);
            replay->actor.update(60 / rate, 1.0f / static_cast<f32>(rate), views);
            REQUIRE(replay->checkpoint->canCommit(*undo));
            REQUIRE(replay->checkpoint->commitRestore(std::move(*undo)));
            CHECK(capture(*replay->checkpoint) == expected);
        }
    }
    CHECK(changed);
    CHECK(replay->device.texturesCreated == textures);
    CHECK(replay->device.textureUpdates == 0);
    CHECK(replay->device.draws.empty());
}

TEST_CASE("combatant restore binds native child poses and auxiliary clocks without spawning",
          "[combatant-checkpoint][assets]") {
    const auto* const name = GENERATE("CHIMERA", "LICH");
    auto first = std::make_unique<Fighter>(bossDefinition(name));
    auto replay = std::make_unique<Fighter>(bossDefinition(name));
    const auto views = std::array{visiblePlayer(3, {2, 0, 15}), visiblePlayer(1, {-9, 0, 11})};
    for (s32 frame = 0; frame < 45; ++frame) {
        first->actor.update(1, 1.0f / 60, views);
    }
    const std::array arena{CombatArenaTarget{2, glm::translate(Mat4{1}, Vec3{3, 0, 8}), false},
                           CombatArenaTarget{5, glm::translate(Mat4{1}, Vec3{-3, 0, 8}), true}};
    first->actor.setArenaTargets(arena);
    first->actor.setArenaAnchors(std::array{Mat4{1}});
    const auto baseline = capture(*first->checkpoint);
    REQUIRE(replay->checkpoint->restore(baseline));
    if (std::string_view(name) == "CHIMERA") {
        REQUIRE(baseline.parts.size() == 4);
    } else {
        REQUIRE(baseline.parts.front().actor.attachments.size() == 1);
    }
    for (s32 frame = 0; frame < 80; ++frame) {
        CAPTURE(name, frame);
        first->actor.update(1, 1.0f / 60, views);
        replay->actor.update(1, 1.0f / 60, views);
        CHECK(capture(*replay->checkpoint) == capture(*first->checkpoint));
    }
    auto malformed = baseline;
    malformed.parts.front().actor.player.sequence = 999999;
    const auto before = capture(*replay->checkpoint);
    CHECK_FALSE(replay->checkpoint->restore(malformed));
    CHECK(capture(*replay->checkpoint) == before);
}

TEST_CASE("native actor reuse can be rewound without erasing queued events or incarnation",
          "[combatant-checkpoint][assets]") {
    auto fixture = std::make_unique<Fighter>(Golem::definition());
    const auto baseline = capture(*fixture->checkpoint);
    auto undo = fixture->checkpoint->prepareRestore(baseline);
    REQUIRE(undo);
    REQUIRE(fixture->actor.spawn(fixture->stock, 40, {4, 0, 8}, 1, nullptr, {}, 'G'));
    CHECK(fixture->actor.incarnation() == baseline.parts.front().incarnation + 1);
    REQUIRE(fixture->checkpoint->canCommit(*undo));
    REQUIRE(fixture->checkpoint->commitRestore(std::move(*undo)));
    CHECK(capture(*fixture->checkpoint) == baseline);
}

TEST_CASE("combatant checkpoint retains canonical model overrides without loading them at restore",
          "[combatant-checkpoint][assets]") {
    auto first = std::make_unique<Fighter>(Golem::definition());
    auto replay = std::make_unique<Fighter>(Golem::definition());
    const auto found = std::ranges::find_if(first->stock.tree->nodes, [&](const auto& node) {
        return !node.object.empty() && first->stock.archive.models.find(node.object).has_value();
    });
    REQUIRE(found != first->stock.tree->nodes.end());
    REQUIRE(first->actor.replaceNodeModel(first->device, found->name, found->object));
    const auto baseline = capture(*first->checkpoint);
    REQUIRE(baseline.parts.front().actor.replacements.size() == 1);
    // No packet-selected resource loading: an unprepared destination refuses.
    const auto untouched = capture(*replay->checkpoint);
    const u32 coldUploads = replay->device.texturesCreated;
    CHECK_FALSE(replay->checkpoint->restore(baseline));
    CHECK(capture(*replay->checkpoint) == untouched);
    CHECK(replay->device.texturesCreated == coldUploads);
    REQUIRE(replay->actor.replaceNodeModel(replay->device, found->name, found->object));
    REQUIRE(replay->actor.replaceNodeModel(replay->device, found->name, ""));
    const u32 warmUploads = replay->device.texturesCreated;
    REQUIRE(replay->checkpoint->restore(baseline));
    CHECK(capture(*replay->checkpoint) == baseline);
    CHECK(replay->device.texturesCreated == warmUploads);
}

TEST_CASE("native broken hit-node poses and queued secondary shots survive correction",
          "[combatant-checkpoint][assets]") {
    auto first = std::make_unique<Fighter>(bossDefinition("GARM"));
    auto replay = std::make_unique<Fighter>(bossDefinition("GARM"));
    const auto parts = first->stock.data.parts();
    const auto found = std::ranges::find_if(parts, [](const auto& part) {
        return (part.flags & CritterPart::kBreakable) != 0 && part.healthScale > 0 &&
               part.damageScale > 0;
    });
    REQUIRE(found != parts.end());
    EnemyHit hit;
    hit.player = 3;
    hit.damage = 100000;
    hit.level = 99;
    hit.node = static_cast<s32>(found - parts.begin());
    first->actor.hurt(hit);
    const auto baseline = capture(*first->checkpoint);
    const auto& node = baseline.parts.front().actor.hitNodes[static_cast<usize>(hit.node)];
    REQUIRE(node.broken);
    REQUIRE(node.heldPose);
    REQUIRE(replay->checkpoint->restore(baseline));
    CHECK(capture(*replay->checkpoint) == baseline);
    const auto players = std::array{visiblePlayer(3, {1, 0, 10})};
    for (s32 tick = 0; tick < 20; ++tick) {
        first->actor.update(1, 1.0f / 60, players);
        replay->actor.update(1, 1.0f / 60, players);
        CHECK(capture(*replay->checkpoint) == capture(*first->checkpoint));
    }
}

TEST_CASE("combatant arena random state resumes the next native selection after slot reuse",
          "[combatant-checkpoint][assets]") {
    auto first = std::make_unique<Fighter>(Golem::definition());
    auto replay = std::make_unique<Fighter>(Golem::definition());
    const std::array arena{CombatArenaTarget{0, Mat4{1}, false},
                           CombatArenaTarget{1, Mat4{1}, true},
                           CombatArenaTarget{2, Mat4{1}, false}};
    first->actor.setArenaTargets(arena);
    const auto baseline = capture(*first->checkpoint);
    REQUIRE(baseline.parts.front().arenaRandom != 1);
    REQUIRE(replay->checkpoint->restore(baseline));
    for (s32 count = 0; count < 8; ++count) {
        REQUIRE(first->actor.spawn(first->stock, 40, {}, 0, nullptr, {}, 'G'));
        REQUIRE(replay->actor.spawn(replay->stock, 40, {}, 0, nullptr, {}, 'G'));
        first->actor.setArenaTargets(arena);
        replay->actor.setArenaTargets(arena);
        CHECK(capture(*replay->checkpoint) == capture(*first->checkpoint));
    }
}

TEST_CASE("clearing and reloading trusted combatant assets never revives an old restore token",
          "[combatant-checkpoint][assets]") {
    auto fixture = std::make_unique<Fighter>(Golem::definition());
    const auto baseline = capture(*fixture->checkpoint);
    auto prepared = fixture->checkpoint->prepareRestore(baseline);
    REQUIRE(prepared);
    fixture->stock.clear();
    CHECK_FALSE(fixture->checkpoint->canCommit(*prepared));
    CHECK_FALSE(fixture->checkpoint->checkpoint());
    const auto root = test::assetOrSkip("CRITTER/GOLEM.WAD").parent_path().parent_path();
    REQUIRE(fixture->stock.load(fixture->device, root, Golem::definition(), 'G'));
    CHECK_FALSE(fixture->checkpoint->canCommit(*prepared));
    CHECK_FALSE(fixture->checkpoint->restore(baseline));
    // Never update/draw the now-invalid live actor: the owning stage clears its
    // borrowers before resource reload. The stale-token checks do not dereference
    // its old definition/pose pointers.
    fixture->actor.clear();
}
} // namespace
