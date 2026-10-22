#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/core/Types.h"

#include "TestSupport.h"
#include "game/enemies/EnemyAnimator.h"
#include "game/enemies/EnemyMind.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Action = EnemyAction;

constexpr s32 kTicks = 2;
constexpr f32 kStep = 1.0f / 30.0f;

/** A grunt's sequences: no DEATH, no ATTACK2, a ready-to-walk in and out. */
TreeInfo gruntTree() {
    TreeInfo tree;
    tree.name = "GRU1";
    TreeNodeInfo root;
    root.name = "OANIM";
    root.type = 2;
    tree.nodes.push_back(root);
    struct Entry {
        const char* name;
        s32 frames;
        bool repeats;
    };
    const std::array<Entry, 12> entries{{{"READY", 20, true},
                                         {"WALK", 16, true},
                                         {"ATTACK1", 8, false},
                                         {"ATTACK1R", 8, false},
                                         {"ATTACK3", 10, false},
                                         {"ATTACK3R", 10, false},
                                         {"START", 12, false},
                                         {"HIT1", 6, false},
                                         {"HIT2", 12, false},
                                         {"GETUP", 10, false},
                                         {"READYTOWALK", 4, false},
                                         {"WALKTOREADY", 4, false}}};
    for (const Entry& entry : entries) {
        TreeSequenceInfo sequence;
        sequence.name = entry.name;
        sequence.frames = entry.frames;
        sequence.frameRate = 30;
        sequence.repeats = entry.repeats;
        sequence.trackOfNode = {-1};
        tree.sequences.push_back(sequence);
    }
    return tree;
}

s32 stepsUntil(EnemyAnimator& animator, Action ask, Action wanted, s32 limit) {
    s32 steps = 0;
    while (animator.action() != wanted && steps < limit) {
        animator.request(ask);
        animator.update(kTicks, kStep);
        ++steps;
    }
    return steps;
}

TEST_CASE("a collision hold cancels pending walk and run without interrupting other requests",
          "[enemies][animation][alpha-seek-blocked]") {
    // fn_8004D030 assigns daction=READY only for WALK/RUN. A low-priority
    // request(READY) cannot replace those, and must not cancel an attack or hit.
    const auto tree = gruntTree();
    for (usize i = 0; i < kEnemyActionCount; ++i) {
        EnemyAnimator animator;
        REQUIRE(animator.bind(tree));
        const auto action = static_cast<Action>(i);
        CAPTURE(i);
        animator.request(action);
        animator.stopWalking();
        CHECK(animator.requested() ==
              (action == Action::Walk || action == Action::Run ? Action::Ready : action));
        CHECK(animator.action() == Action::Start); // no animation step or restart
    }
}

TEST_CASE("native enemy animation equality covers requests clocks unbound values and correction",
          "[enemies][animation][local-rewind][native-equality]") {
    EnemyAnimator animator;
    const EnemyAnimator empty = animator;
    CHECK(animator.equalLocal(empty));
    animator.setIdle(0.25f);
    CHECK_FALSE(animator.equalLocal(empty)); // unbound does not mean valueless
    animator = empty;
    const auto tree = gruntTree();
    REQUIRE(animator.bind(tree));
    const EnemyAnimator initial = animator;
    const auto changed = [&](auto&& change) {
        animator = initial;
        change();
        CHECK_FALSE(animator.equalLocal(initial));
    };
    changed([&] { animator.request(Action::Attack); });
    changed([&] { animator.setIdle(0.125f); });
    changed([&] { animator.setThrowInterval(0.5f); });
    changed([&] { animator.update(1, 1.0f / 60); });

    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization): a distinct copy must compare
    // unequal
    const auto foreignTree = tree;
    EnemyAnimator foreign;
    REQUIRE(foreign.bind(foreignTree));
    CHECK_FALSE(initial.equalLocal(foreign));

    animator = initial;
    std::array<EnemyAnimator, 8> uninterrupted;
    for (auto& expected : uninterrupted) {
        animator.request(Action::Walk);
        animator.update(1, 1.0f / 60);
        expected = animator;
    }
    animator = initial;
    for (usize tick = 0; tick < uninterrupted.size(); ++tick) {
        animator.request(Action::Dying);
        animator.update(1, 1.0f / 60);
    }
    CHECK_FALSE(animator.equalLocal(uninterrupted.back()));
    animator = initial;
    for (const auto& expected : uninterrupted) {
        animator.request(Action::Walk);
        animator.update(1, 1.0f / 60);
        CHECK(animator.equalLocal(expected));
    }
}

TEST_CASE("enemy presentation interpolates without changing attack events and holds across cuts",
          "[enemies][animation][presentation]") {
    TreeInfo tree = gruntTree();
    for (auto& sequence : tree.sequences) {
        TrackInfo track;
        track.node = 0;
        track.flags = TrackInfo::channelBit(3);
        track.frames = {0, 60};
        track.values = {0, 60};
        sequence.tracks = {track};
        sequence.trackOfNode = {0};
    }
    EnemyAnimator native;
    EnemyAnimator shown;
    REQUIRE(native.bind(tree));
    REQUIRE(shown.bind(tree));
    TreePose pose;
    native.update(kTicks, kStep);
    shown.update(kTicks, kStep);
    for (const f32 fraction : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        shown.evaluatePresentation(pose, fraction);
        CHECK(pose.matrices()[0][3].x == Catch::Approx(fraction));
        CHECK(shown.presentationFrame(fraction) == Catch::Approx(fraction));
    }
    s32 blows = 0;
    for (s32 step = 0; step < 150; ++step) {
        native.request(Action::Attack);
        shown.request(Action::Attack);
        native.update(kTicks, kStep);
        shown.update(kTicks, kStep);
        for (const f32 fraction : {0.0f, 0.5f, 1.0f}) {
            shown.evaluatePresentation(pose, fraction);
        }
        CHECK(shown.player().frame() == native.player().frame());
        CHECK(shown.pose().matrices()[0] == native.pose().matrices()[0]);
        CHECK(shown.struck() == native.struck());
        CHECK(shown.threw() == native.threw());
        blows += shown.struck() ? 1 : 0;
    }
    CHECK(blows > 0);
    shown.holdPresentation();
    shown.evaluatePresentation(pose, 0);
    const Mat4 held = pose.matrices()[0];
    shown.evaluatePresentation(pose, 1);
    CHECK(pose.matrices()[0] == held);
    shown.request(Action::Dying);
    shown.update(kTicks, kStep);
    shown.evaluatePresentation(pose, 0);
    const Mat4 cut = pose.matrices()[0];
    shown.evaluatePresentation(pose, 1);
    CHECK(pose.matrices()[0] == cut);
}

TEST_CASE("captured enemy updates retain standalone actions and held presentation intervals",
          "[enemies][animation][presentation][captured-update]") {
    TreeInfo tree = gruntTree();
    for (const auto* name : {"THROW1", "THROW2", "THROWF", "ATTTOREADY"}) {
        TreeSequenceInfo sequence = tree.sequences.front();
        sequence.name = name;
        sequence.frames = 3;
        sequence.repeats = false;
        tree.sequences.push_back(sequence);
    }
    for (usize index = 0; index < tree.sequences.size(); ++index) {
        auto& sequence = tree.sequences[index];
        TrackInfo track;
        track.node = 0;
        track.flags = TrackInfo::channelBit(3);
        track.frames = {0, 60};
        const f32 offset = static_cast<f32>(index) * 10;
        track.values = {offset, offset + 60};
        sequence.tracks = {track};
        sequence.trackOfNode = {0};
    }

    bool sawStrike = false;
    bool sawPower = false;
    bool sawThrow = false;
    bool sawDead = false;
    for (const auto action : {Action::Walk, Action::Attack, Action::PowerAttack, Action::Throw,
                              Action::HitReact1, Action::HitReact2, Action::Dying}) {
        CAPTURE(action);
        EnemyAnimator standalone;
        REQUIRE(standalone.bind(tree));
        EnemyAnimator previousOwner = standalone;
        EnemyAnimator captured = standalone;
        for (s32 tick = 0; tick < 240; ++tick) {
            CAPTURE(tick);
            // Enemies accumulate two 60 Hz ticks before stepping their animator.
            // Stopped/off-screen owners still capture, but do not animate at all.
            const bool stopped = tick >= 30 && tick < 40;
            if (stopped) {
                standalone.holdPresentation();
                previousOwner.holdPresentation();
                captured.holdPresentation();
            } else if ((tick % 2) == 0) {
                previousOwner.holdPresentation();
                captured.holdPresentation();
                for (auto* animator : {&standalone, &previousOwner, &captured}) {
                    // The native owner asks its mind and applies collision holds
                    // after capturing, before any animation advances.
                    animator->request(action);
                    if ((tick % 34) == 0) {
                        animator->stopWalking();
                    }
                }
                const bool contact = (tick % 10) == 0;
                standalone.update(kTicks, kStep, contact);
                previousOwner.update(kTicks, kStep, contact); // original duplicate capture
                captured.updateCaptured(kTicks, kStep, contact);
                sawStrike |= captured.struck();
                sawPower |= captured.powerStruck();
                sawThrow |= captured.threw();
                sawDead |= captured.dead();
            }
            CHECK(captured.equalLocal(previousOwner));
            CHECK(captured.equalLocal(standalone));
            for (const f32 alpha : {0.0f, 0.37f, 1.0f}) {
                TreePose expected;
                TreePose actual;
                standalone.evaluatePresentation(expected, alpha);
                captured.evaluatePresentation(actual, alpha);
                CHECK(actual.equalLocal(expected));
                CHECK(captured.presentationFrame(alpha) == standalone.presentationFrame(alpha));
            }
        }
    }
    CHECK(sawStrike);
    CHECK(sawPower);
    CHECK(sawThrow);
    CHECK(sawDead);
}

TEST_CASE("captured enemy updates preserve unbound early outs and native correction copies",
          "[enemies][animation][local-rewind][native-equality][captured-update]") {
    EnemyAnimator standalone;
    EnemyAnimator captured;
    standalone.setIdle(0.25f);
    captured.setIdle(0.25f);
    captured.holdPresentation();
    standalone.request(Action::Dying);
    captured.request(Action::Dying);
    standalone.update(kTicks, kStep);
    captured.updateCaptured(kTicks, kStep);
    REQUIRE(captured.equalLocal(standalone));

    const auto tree = gruntTree();
    REQUIRE(standalone.bind(tree));
    REQUIRE(stepsUntil(standalone, Action::Walk, Action::Walk, 60) < 60);
    const EnemyAnimator saved = standalone;
    for (usize distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        std::array<EnemyAnimator, 8> expected;
        standalone = saved;
        captured = saved;
        for (usize tick = 0; tick < distance; ++tick) {
            standalone.request(Action::Attack);
            standalone.update(kTicks, kStep);
            expected[tick] = standalone;
            captured.holdPresentation();
            captured.request(Action::Dying);
            captured.updateCaptured(kTicks, kStep);
        }
        REQUIRE_FALSE(captured.equalLocal(standalone));
        captured = saved; // local rewind is a native value copy, not diagnostic restore
        for (usize tick = 0; tick < distance; ++tick) {
            CAPTURE(tick);
            captured.holdPresentation();
            captured.request(Action::Attack);
            captured.updateCaptured(kTicks, kStep);
            CHECK(captured.equalLocal(expected[tick]));
        }
    }
}

TEST_CASE("an enemy walks in, is asked by priority, and lands its blow as the swing ends",
          "[game][enemies][animation]") {
    const TreeInfo tree = gruntTree();
    EnemyAnimator animator;
    REQUIRE(animator.bind(tree, true));
    REQUIRE(animator.entering());
    REQUIRE(animator.has(Action::Attack));
    REQUIRE_FALSE(animator.has(Action::Dying));
    REQUIRE_FALSE(animator.has(Action::Attack2));
    // The entrance plays out whatever is asked, then a grunt goes straight to walking.
    REQUIRE(stepsUntil(animator, Action::Walk, Action::Walk, 30) < 30);
    REQUIRE(animator.moving());
    // Left alone it comes to rest by way of the walk-to-ready.
    REQUIRE(stepsUntil(animator, Action::Ready, Action::WalkToReady, 30) < 30);
    REQUIRE(stepsUntil(animator, Action::Ready, Action::Ready, 30) < 30);
    // A louder request wins the tick; a quieter one is refused.
    animator.request(Action::Walk);
    animator.request(Action::Attack);
    REQUIRE(animator.requested() == Action::Attack);
    animator.request(Action::Walk);
    REQUIRE(animator.requested() == Action::Attack);
    animator.request(Action::HitReact1);
    REQUIRE(animator.requested() == Action::HitReact1);
    animator.update(kTicks, kStep);
    REQUIRE(animator.action() == Action::HitReact1);
    REQUIRE(animator.reacting());
    REQUIRE(animator.requested() == Action::Ready); // forgotten with the tick
    REQUIRE(stepsUntil(animator, Action::Ready, Action::Ready, 30) < 30);
    // An attack asked from the stance waits for the stance's loop to end, then swings; the
    // blow lands as the swing gives way to its recovery, once.
    REQUIRE(stepsUntil(animator, Action::Attack, Action::Attack, 60) < 60);
    REQUIRE(animator.swinging());
    REQUIRE(animator.attacking());
    s32 struck = 0;
    for (s32 i = 0; i < 40; ++i) {
        animator.request(Action::Ready);
        animator.update(kTicks, kStep);
        struck += animator.struck() ? 1 : 0;
        if (animator.struck()) {
            REQUIRE(animator.action() == Action::AttackRecover);
        }
    }
    REQUIRE(struck == 1);
    REQUIRE(animator.action() == Action::Ready);
    // The power blow is told apart.
    REQUIRE(stepsUntil(animator, Action::PowerAttack, Action::PowerAttack, 60) < 60);
    s32 power = 0;
    for (s32 i = 0; i < 40; ++i) {
        animator.update(kTicks, kStep);
        power += animator.powerStruck() ? 1 : 0;
        REQUIRE_FALSE(animator.struck());
    }
    REQUIRE(power == 1);
    // A hit cuts into a swing at once; the knock-down gets up again.
    REQUIRE(stepsUntil(animator, Action::Attack, Action::Attack, 60) < 60);
    animator.request(Action::HitReact2);
    animator.update(kTicks, kStep);
    REQUIRE(animator.action() == Action::HitReact2);
    REQUIRE(stepsUntil(animator, Action::Ready, Action::GetUp, 40) < 40);
    REQUIRE(stepsUntil(animator, Action::Ready, Action::Ready, 40) < 40);
    // A death the tree lacks plays as the knock-down, and is done when that is.
    animator.request(Action::Dying);
    animator.update(kTicks, kStep);
    REQUIRE(animator.dying());
    REQUIRE_FALSE(animator.dead());
    REQUIRE(animator.player().sequence() == animator.sequenceOf(Action::HitReact2));
    s32 until = 0;
    while (!animator.dead() && until < 60) {
        animator.request(Action::Dying);
        animator.update(kTicks, kStep);
        ++until;
    }
    REQUIRE(animator.dead());
    REQUIRE(until >= 5);
}

TEST_CASE("a walk pending as the tick's default refuses the stance and, as loud, a run",
          "[game][enemies][animation]") {
    // The scorpion's default action is the walk (do_enemies), which RequestEnemyAction's
    // priorities then hold against the stance and a run, though not against an attack.
    const TreeInfo tree = gruntTree();
    EnemyAnimator animator;
    REQUIRE(animator.bind(tree, true));
    animator.request(Action::Walk);
    animator.request(Action::Ready);
    CHECK(animator.requested() == Action::Walk);
    animator.request(Action::Run);
    CHECK(animator.requested() == Action::Walk);
    animator.request(Action::Attack);
    CHECK(animator.requested() == Action::Attack);
}

TEST_CASE("a body idling after a throw refuses to attack, and a tree without a stance is refused",
          "[game][enemies][animation]") {
    TreeInfo tree = gruntTree();
    EnemyAnimator animator;
    REQUIRE(animator.bind(tree, false));
    animator.setIdle(1.0f);
    animator.request(Action::Attack);
    REQUIRE(animator.requested() == Action::Ready);
    animator.request(Action::Walk);
    REQUIRE(animator.requested() == Action::Walk);
    for (s32 i = 0; i < 40; ++i) {
        animator.update(kTicks, kStep);
    }
    REQUIRE(animator.idleSeconds() == 0.0f);
    animator.request(Action::Attack);
    REQUIRE(animator.requested() == Action::Attack);
    animator.unbind();
    REQUIRE_FALSE(animator.bound());
    tree.sequences.erase(tree.sequences.begin());
    REQUIRE_FALSE(animator.bind(tree));
}

TEST_CASE("repeated death requests finish once and never stand back up",
          "[game][enemies][animation][enemy-feedback]") {
    for (const bool deathSequence : {false, true}) {
        TreeInfo tree = gruntTree();
        if (deathSequence) {
            TreeSequenceInfo sequence = tree.sequences[8];
            sequence.name = "DEATH";
            tree.sequences.push_back(sequence);
        }
        EnemyAnimator animator;
        REQUIRE(animator.bind(tree, false));
        animator.request(Action::HitReact2);
        animator.update(kTicks, kStep);
        REQUIRE(animator.action() == Action::HitReact2);
        animator.request(Action::Dying);
        animator.update(kTicks, kStep);
        REQUIRE(animator.action() == Action::Dying);
        for (s32 frame = 0; frame < 30; ++frame) {
            animator.request(Action::Dying);
            animator.update(kTicks, kStep);
            CHECK(animator.action() == Action::Dying);
            CHECK_FALSE(animator.struck());
        }
        CHECK(animator.dead());
    }
}

TEST_CASE("throw stages carry fractional retail waits without blocking movement or reactions",
          "[game][enemies][animation][battle-archer]") {
    TreeInfo tree = gruntTree();
    for (const auto* name : {"THROW1", "THROW2", "THROWF", "ATTTOREADY"}) {
        TreeSequenceInfo sequence = tree.sequences.front();
        sequence.name = name;
        sequence.frames = 3;
        sequence.repeats = false;
        tree.sequences.push_back(sequence);
    }
    EnemyAnimator animator;
    REQUIRE(animator.bind(tree));
    REQUIRE(stepsUntil(animator, Action::Ready, Action::Ready, 30) < 30);
    SECTION("an exact second stays in the carry until the next throw stage") {
        animator.setThrowInterval(1.0f);
        REQUIRE(stepsUntil(animator, Action::Throw, Action::Throw, 30) < 30);
        CHECK(animator.idleSeconds() == 0.0f);
        REQUIRE(stepsUntil(animator, Action::Throw, Action::ThrowFinish, 30) < 30);
        CHECK(animator.threw());
        CHECK(animator.idleSeconds() == Catch::Approx(1.1f));
        animator.request(Action::Throw);
        CHECK(animator.requested() == Action::Ready);
        animator.request(Action::RunAttack);
        CHECK(animator.requested() == Action::RunAttack);
        animator.request(Action::HitReact1);
        animator.update(kTicks, kStep);
        CHECK(animator.action() == Action::HitReact1);
        REQUIRE(stepsUntil(animator, Action::Ready, Action::Ready, 60) < 60);
        for (s32 frame = 0; frame < 40; ++frame) {
            animator.update(kTicks, kStep);
        }
        REQUIRE(stepsUntil(animator, Action::Throw, Action::Throw, 30) < 30);
    }
    SECTION("quarter seconds accumulate at stage changes, not every animation tick") {
        animator.setThrowInterval(0.25f);
        for (const Action next :
             {Action::Throw, Action::ThrowFinish, Action::Throw2, Action::ThrowFinish}) {
            REQUIRE(stepsUntil(animator, Action::Throw, next, 30) < 30);
            CHECK(animator.idleSeconds() == 0.0f);
        }
        REQUIRE(stepsUntil(animator, Action::Throw, Action::Throw2, 30) < 30);
        CHECK(animator.idleSeconds() == Catch::Approx(1.1f));
    }
    SECTION("the placement's no-wait interval never adds a pause") {
        animator.setThrowInterval(0.0f);
        for (s32 frame = 0; frame < 90; ++frame) {
            animator.request(Action::Throw);
            animator.update(kTicks, kStep);
            CHECK(animator.idleSeconds() == 0.0f);
        }
    }
    // Reusing an animator must not inherit either the interval or its fractional carry.
    REQUIRE(animator.bind(tree));
    REQUIRE(stepsUntil(animator, Action::Ready, Action::Ready, 30) < 30);
    REQUIRE(stepsUntil(animator, Action::Throw, Action::Throw, 30) < 30);
    CHECK(animator.idleSeconds() == 0.0f);
}

TEST_CASE("the Province suicide bomber stays still throughout its authored running windup",
          "[game][enemies][animation][alpha-bomber-start][assets]") {
    const auto path = test::assetOrSkip("MONSTERS/GRU/ANIM.PS2");
    AnimationSet actions;
    REQUIRE(actions.load(path.parent_path()));
    const auto found = actions.find("GRUS");
    REQUIRE(found);
    const auto& tree = actions.tree(*found);
    const auto windup = tree.findSequence("READYTOWALK");
    REQUIRE(windup);
    REQUIRE(tree.findSequence("RUN"));
    // The authored bomber has no WALK, so action 9 finishes into RUN (4).
    REQUIRE_FALSE(tree.findSequence("WALK"));
    EnemyAnimator animator;
    REQUIRE(animator.bind(tree));
    MindMemory memory;
    MindSense sense;
    sense.target = 0;
    sense.targetPosition = {0, 0, 12};
    sense.targetDistance = 12;
    sense.recognized = true;
    sense.ticks = kTicks;
    const auto& mind = enemyMindOf(kSuicideWay);
    bool sawWindup = false;
    s32 windupUpdates = 0;
    for (s32 tick = 0; tick < 180 && animator.action() != Action::Run; ++tick) {
        sense.action = animator.action();
        const auto intent = mind.think(memory, sense);
        CHECK(intent.pace == 0);
        CHECK_FALSE(intent.yell);
        if (sense.action == Action::ReadyToWalk) {
            sawWindup = true;
            ++windupUpdates;
            CHECK(animator.player().sequence() == *windup);
        }
        animator.request(intent.action);
        animator.update(kTicks, kStep);
    }
    REQUIRE(sawWindup);
    REQUIRE(windupUpdates > 1);
    REQUIRE(animator.action() == Action::Run);
    sense.action = animator.action();
    const auto run = mind.think(memory, sense);
    CHECK(run.yell);
    CHECK(run.pace == 1.5f);
    CHECK(run.action == Action::Run);
    CHECK(memory.counter == kTicks);
}

} // namespace
