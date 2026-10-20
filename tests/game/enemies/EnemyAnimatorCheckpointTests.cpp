#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"

#include "TestSupport.h"
#include "game/enemies/EnemyAnimator.h"
#include "game/enemies/EnemyKinds.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Action = EnemyAction;
constexpr f32 kStep = 1.0f / 60;

TreeInfo animatedTree() {
    TreeInfo tree;
    tree.name = "CHECKPOINT_ENEMY";
    for (s32 node = 0; node < 3; ++node) {
        TreeNodeInfo info;
        info.name = "NODE" + std::to_string(node);
        info.parent = node - 1;
        info.position = {0.125f, 0.25f * static_cast<f32>(node), -0.5f};
        tree.nodes.push_back(info);
    }
    for (const auto name : EnemyAnimator::kSequenceNames) {
        if (tree.findSequence(name)) {
            continue; // the walking and running transitions deliberately share names
        }
        TreeSequenceInfo sequence;
        sequence.name = name;
        sequence.frames = 5;
        sequence.frameRate = 27;
        const auto offset = static_cast<f32>(tree.sequences.size()) * 0.02f;
        for (u32 node = 0; node < tree.nodes.size(); ++node) {
            TrackInfo track;
            track.node = node;
            track.flags = static_cast<u16>(TrackInfo::kChannels |
                                           ((node % 2) != 0 ? TrackInfo::kPitchYawRoll : 0));
            track.frames = {0, 4};
            track.values = {offset, 0.2f,         -0.3f, offset, 0.5f,         0.75f, 1,    1.125f,
                            0.875f, offset + .4f, 0.6f,  -0.1f,  offset + .5f, 0.6f,  0.8f, .9f,
                            1,      1.1f};
            sequence.trackOfNode.push_back(static_cast<s32>(sequence.tracks.size()));
            sequence.tracks.push_back(track);
        }
        tree.sequences.push_back(sequence);
    }
    return tree;
}

void samePose(const TreePose& actual, const TreePose& expected) {
    CHECK(std::ranges::equal(actual.poses(), expected.poses()));
    CHECK(std::ranges::equal(actual.matrices(), expected.matrices()));
}

void sameAnimator(const EnemyAnimator& actual, const EnemyAnimator& expected) {
    REQUIRE(actual.checkpoint());
    REQUIRE(expected.checkpoint());
    CHECK(actual.checkpoint() == expected.checkpoint());
    samePose(actual.pose(), expected.pose());
    for (const f32 fraction : {0.0f, 0.375f, 1.0f}) {
        TreePose actualPose;
        TreePose expectedPose;
        actual.evaluatePresentation(actualPose, fraction);
        expected.evaluatePresentation(expectedPose, fraction);
        samePose(actualPose, expectedPose);
        CHECK(actual.presentationFrame(fraction) == expected.presentationFrame(fraction));
    }
}

TEST_CASE("enemy checkpoints retain pending actions release latches and exact native poses",
          "[rollback][enemies][enemy-checkpoint]") {
    const auto tree = animatedTree();
    std::array<bool, kEnemyActionCount> requested{};
    bool struck = false;
    bool powerStruck = false;
    bool threw = false;
    bool dead = false;
    for (usize action = 0; action < kEnemyActionCount; ++action) {
        CAPTURE(action);
        EnemyAnimator native;
        EnemyAnimator replay;
        REQUIRE(native.bind(tree, action % 2 != 0));
        REQUIRE(replay.bind(tree, action % 2 == 0));
        native.setThrowInterval(0.25f);
        for (s32 tick = 0; tick < 100; ++tick) {
            CAPTURE(tick);
            const auto next = static_cast<Action>(action);
            native.request(next);
            requested[static_cast<usize>(native.requested())] = true;
            const auto before = native.checkpoint();
            REQUIRE(before);
            REQUIRE(before->valid());
            REQUIRE(replay.restore(*before));
            sameAnimator(replay, native);
            const f32 seconds = tick % 7 == 0 ? kStep * 0.25f : kStep;
            const bool contact = tick % 5 < 3;
            native.update(1, seconds, contact);
            replay.update(1, seconds, contact);
            sameAnimator(replay, native);
            struck = struck || native.struck();
            powerStruck = powerStruck || native.powerStruck();
            threw = threw || native.threw();
            dead = dead || native.dead();
            const auto after = native.checkpoint();
            REQUIRE(after);
            REQUIRE(replay.restore(*after));
            sameAnimator(replay, native);
        }
    }
    CHECK(std::ranges::all_of(requested, [](bool value) { return value; }));
    CHECK(struck);
    CHECK(powerStruck);
    CHECK(threw);
    CHECK(dead);
}

TEST_CASE("enemy checkpoints rewind repeated cuts and carry a throw's exact fractional wait",
          "[rollback][enemies][enemy-checkpoint]") {
    auto tree = animatedTree();
    std::erase_if(tree.sequences, [](const auto& sequence) { return sequence.name == "DEATH"; });
    EnemyAnimator native;
    REQUIRE(native.bind(tree));
    native.setThrowInterval(1);
    std::vector<EnemyAnimator::Checkpoint> history;
    std::vector<Action> inputs;
    bool carriedExactSecond = false;
    bool waited = false;
    bool sawDeathFallback = false;
    for (s32 tick = 0; tick < 250; ++tick) {
        CAPTURE(tick);
        const auto baseline = native.checkpoint();
        REQUIRE(baseline);
        history.push_back(*baseline);
        Action action = Action::Throw;
        if (tick >= 210) {
            action = Action::Dying;
        } else if (tick == 110 || tick == 150) {
            action = Action::HitReact1;
        }
        inputs.push_back(action);
        native.request(action);
        native.update(1, kStep, true);
        const auto state = native.checkpoint();
        REQUIRE(state);
        carriedExactSecond = carriedExactSecond || state->idleFraction == 1;
        waited = waited || state->idleSeconds > 1;
        if (native.dying()) {
            sawDeathFallback = true;
            CHECK(state->player.sequence == native.sequenceOf(Action::HitReact2));
        }
        EnemyAnimator replay;
        REQUIRE(replay.bind(tree));
        const auto start = static_cast<usize>(std::max(tick - 9, 0));
        REQUIRE(replay.restore(history[start]));
        for (usize at = start; at < inputs.size(); ++at) {
            replay.request(inputs[at]);
            replay.update(1, kStep, true);
        }
        sameAnimator(replay, native);
    }
    CHECK(carriedExactSecond);
    CHECK(waited);
    CHECK(sawDeathFallback);
    CHECK(native.dead());
}

TEST_CASE("enemy checkpoint restores held presentation and an exact transition source pose",
          "[rollback][enemies][enemy-checkpoint]") {
    const auto tree = animatedTree();
    EnemyAnimator native;
    REQUIRE(native.bind(tree));
    for (s32 tick = 0; tick < 30; ++tick) {
        native.request(Action::Walk);
        native.update(1, kStep);
    }
    auto source = native.checkpoint();
    REQUIRE(source);
    REQUIRE(source->pose);
    REQUIRE(source->previous);
    REQUIRE(source->presentationBefore);
    // Enemy actions currently cut with a zero transition. The checkpoint retains
    // AnimationPlayer's complete blend state as well, rather than assuming that
    // the previous stance can be reevaluated from the new sequence.
    source->player.transitionLength = 0.2f;
    source->player.transitionTime = 0.05f;
    source->previous->poses[1].position = {0.3f, -0.7f, 0.6f};
    source->pose->poses[2].rotation = {-0.2f, 0.9f, 0.1f};
    source->presentationBefore->poses[0].scale = {0.5f, 0.7f, 1.2f};
    REQUIRE(native.restore(*source));
    EnemyAnimator replay;
    REQUIRE(replay.bind(tree));
    REQUIRE(replay.restore(*source));
    sameAnimator(replay, native);
    for (s32 tick = 0; tick < 30; ++tick) {
        native.request(Action::Walk);
        replay.request(Action::Walk);
        native.update(1, kStep);
        replay.update(1, kStep);
        sameAnimator(replay, native);
    }
    native.holdPresentation();
    REQUIRE(replay.restore(*native.checkpoint()));
    sameAnimator(replay, native);
    TreePose held;
    replay.evaluatePresentation(held, 0);
    TreePose complete;
    replay.evaluatePresentation(complete, 1);
    samePose(held, complete);
}

TEST_CASE("enemy checkpoint refusal preserves the entire previously bound body",
          "[rollback][enemies][enemy-checkpoint]") {
    const auto tree = animatedTree();
    EnemyAnimator native;
    REQUIRE_FALSE(native.checkpoint());
    REQUIRE(native.bind(tree));
    native.request(Action::HitReact1);
    native.update(1, kStep);
    const auto source = native.checkpoint();
    REQUIRE(source);
    const auto reject = [&](const EnemyAnimator::Checkpoint& value) {
        CHECK_FALSE(native.restore(value));
        CHECK(native.checkpoint() == source);
    };
    auto invalid = *source;
    invalid.current = static_cast<Action>(255);
    reject(invalid);
    invalid = *source;
    invalid.requested = static_cast<Action>(255);
    reject(invalid);
    invalid = *source;
    invalid.tree = "DIFFERENT";
    reject(invalid);
    invalid = *source;
    invalid.player.sequence = native.sequenceOf(Action::Walk);
    reject(invalid);
    invalid = *source;
    invalid.player.playing = false;
    reject(invalid);
    invalid = *source;
    invalid.player.frame = AnimationPlayer::kCheckpointFrames;
    reject(invalid);
    invalid = *source;
    invalid.idleSeconds = std::numeric_limits<f32>::quiet_NaN();
    reject(invalid);
    invalid = *source;
    invalid.throwInterval = EnemyAnimator::Checkpoint::kMaxThrowInterval + 1;
    reject(invalid);
    invalid = *source;
    invalid.idleFraction = 1.01f;
    reject(invalid);
    invalid = *source;
    invalid.frameBefore = std::numeric_limits<f32>::infinity();
    reject(invalid);
    invalid = *source;
    invalid.generationBefore = invalid.player.generation + 1;
    reject(invalid);
    invalid = *source;
    invalid.dead = true;
    reject(invalid);
    invalid = *source;
    invalid.pose.reset();
    reject(invalid);
    invalid.previous.reset();
    invalid.presentationBefore.reset();
    reject(invalid);
    invalid = *source;
    invalid.previous->nodes[1].rest.y += 1;
    reject(invalid);
    invalid = *source;
    invalid.presentationBefore->poses[0].scale.x = std::numeric_limits<f32>::quiet_NaN();
    reject(invalid);
    invalid = *source;
    for (auto& pose : invalid.pose->poses) {
        pose.scale = Vec3{TreePose::Checkpoint::kMaxMagnitude};
    }
    reject(invalid); // finite individual channels, unsafe composed hierarchy
    auto differentTree = tree;
    differentTree.nodes[1].name = "REPLACED_NODE";
    EnemyAnimator other;
    REQUIRE(other.bind(differentTree));
    const auto otherBefore = other.checkpoint();
    REQUIRE(otherBefore);
    CHECK_FALSE(other.restore(*source));
    CHECK(other.checkpoint() == otherBefore);
    native.unbind();
    CHECK_FALSE(native.restore(*source));
    CHECK_FALSE(native.checkpoint());
}

TEST_CASE("the invisible IT body checkpoints without inventing a skeleton",
          "[rollback][enemies][enemy-checkpoint]") {
    TreeInfo tree;
    tree.name = "IT";
    TreeSequenceInfo ready;
    ready.name = "READY";
    ready.frames = 1;
    ready.frameRate = 30;
    ready.repeats = true;
    tree.sequences.push_back(ready);
    EnemyAnimator native;
    EnemyAnimator replay;
    REQUIRE(native.bind(tree));
    REQUIRE(replay.bind(tree));
    for (s32 tick = 0; tick < 40; ++tick) {
        native.request(tick < 20 ? Action::Walk : Action::Dying);
        const auto state = native.checkpoint();
        REQUIRE(state);
        CHECK_FALSE(state->pose);
        CHECK_FALSE(state->previous);
        CHECK_FALSE(state->presentationBefore);
        REQUIRE(replay.restore(*state));
        native.update(1, kStep);
        replay.update(1, kStep);
        sameAnimator(replay, native);
    }
    CHECK(native.dead());
}

TEST_CASE("native swarm families resume all body state from delayed checkpoints",
          "[rollback][enemies][enemy-checkpoint][assets]") {
    const auto monsters = test::assetOrSkip("MONSTERS");
    s32 families = 0;
    s32 bodies = 0;
    for (s32 kind = 0; kind <= kDeathKind; ++kind) {
        if (kind >= kSwarmKindCount && kind != kDeathKind) {
            continue;
        }
        const auto& info = enemyKind(kind);
        CAPTURE(info.name);
        AnimationSet assets;
        REQUIRE(assets.load(monsters / std::string(info.name)));
        s32 found = 0;
        for (const auto* const suffix : {"1", "2", "3", "A", "B", "S"}) {
            const auto treeIndex = assets.find(std::string(info.prefix) + suffix);
            if (!treeIndex) {
                continue; // native families deliberately omit unsupported tiers/variants
            }
            const auto& tree = assets.tree(*treeIndex);
            CAPTURE(tree.name);
            EnemyAnimator native;
            REQUIRE(native.bind(tree, (kind % 2) != 0));
            native.setThrowInterval(0.25f);
            std::vector<EnemyAnimator::Checkpoint> checkpoints;
            std::vector<Action> inputs;
            for (s32 tick = 0; tick < 160; ++tick) {
                CAPTURE(tick);
                const auto checkpoint = native.checkpoint();
                REQUIRE(checkpoint);
                REQUIRE(checkpoint->valid());
                checkpoints.push_back(*checkpoint);
                constexpr std::array kActions{Action::Walk,  Action::Attack,    Action::PowerAttack,
                                              Action::Throw, Action::RunAttack, Action::HitReact2,
                                              Action::Walk,  Action::Dying};
                const auto action = kActions[static_cast<usize>(tick / 20)];
                inputs.push_back(action);
                native.request(action);
                native.update(2, kStep * 2, tick % 3 != 0);
                if (tick % 8 != 0 && tick != 159) {
                    continue;
                }
                EnemyAnimator replay;
                REQUIRE(replay.bind(tree));
                const auto start = static_cast<usize>(std::max(tick - 5, 0));
                REQUIRE(replay.restore(checkpoints[start]));
                for (usize at = start; at < inputs.size(); ++at) {
                    replay.request(inputs[at]);
                    replay.update(2, kStep * 2, at % 3 != 0);
                }
                sameAnimator(replay, native);
            }
            ++found;
            ++bodies;
        }
        REQUIRE(found > 0);
        ++families;
    }
    CHECK(families == kSwarmKindCount + 1);
    CHECK(bodies >= families);
}
} // namespace
