#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/PlayerActionCheckpoint.h"
#include "game/netplay/PlayerPoseCheckpoint.h"
#include "game/players/NameCheats.h"
#include "game/world/PlayerFigure.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr f32 kStep = 1.0f / 60;

TreeInfo poseTree() {
    TreeInfo tree;
    tree.name = "POSE";
    for (s32 node = 0; node < 3; ++node) {
        TreeNodeInfo info;
        info.name = "NODE" + std::to_string(node);
        info.parent = node - 1;
        info.position = {0.125f * static_cast<f32>(node), 0.25f, -0.5f};
        tree.nodes.push_back(info);
    }
    for (const auto name : PlayerAnimator::kSequenceNames) {
        TreeSequenceInfo sequence;
        sequence.name = name;
        sequence.frames = 9;
        sequence.frameRate = 24;
        const auto offset = static_cast<f32>(tree.sequences.size()) * 0.03125f;
        for (u32 node = 0; node < tree.nodes.size(); ++node) {
            TrackInfo track;
            track.node = node;
            track.flags = static_cast<u16>(TrackInfo::kChannels |
                                           ((node % 2) != 0 ? TrackInfo::kPitchYawRoll : 0));
            track.frames = {0, 8};
            track.values = {
                offset,        0.2f, -0.3f, offset,        0.5f, 0.75f, 1,    1.125f, 0.875f,
                offset + 0.4f, 0.6f, -0.1f, offset + 0.5f, 0.6f, 0.8f,  0.9f, 1,      1.1f};
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

void sameAnimator(const PlayerAnimator& actual, const PlayerAnimator& expected) {
    CHECK(actual.checkpoint() == expected.checkpoint());
    CHECK(actual.transitionPose() == expected.transitionPose());
    samePose(actual.pose(), expected.pose());
    TreePose actualPresentation;
    TreePose expectedPresentation;
    actual.evaluatePresentation(actualPresentation);
    expected.evaluatePresentation(expectedPresentation);
    samePose(actualPresentation, expectedPresentation);
}

void reachBlend(PlayerAnimator& player) {
    for (s32 tick = 0; tick < 8; ++tick) {
        player.update(PlayerMotion::Run, 1, kStep);
    }
    for (s32 tick = 0; tick < 120 && !player.player().transitioning(); ++tick) {
        player.update(PlayerMotion::Stand, 1, kStep);
    }
    REQUIRE(player.player().transitioning());
}

std::optional<PlayerAnimator::TransitionPose> transported(const PlayerAnimator& player) {
    const auto source = player.transitionPose();
    if (!source) {
        REQUIRE_FALSE(player.player().transitioning());
        return std::nullopt;
    }
    const auto bytes = PlayerPoseCheckpoint::encode(*source);
    REQUIRE(bytes);
    CHECK(bytes->size() <= PlayerPoseCheckpoint::kMaxBytes);
    auto result = PlayerPoseCheckpoint::decode(*bytes);
    REQUIRE(result);
    CHECK(*result == *source);
    return result;
}

PlayerAnimator::Checkpoint transportedClock(const PlayerAnimator& player) {
    const auto bytes = PlayerActionCheckpoint::encode(player.checkpoint());
    REQUIRE(bytes);
    const auto decoded = PlayerActionCheckpoint::decode(*bytes);
    REQUIRE(decoded);
    return *decoded;
}

void word(std::vector<u8>& bytes, usize offset, u32 value) {
    for (usize index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<u8>(value >> (index * 8));
    }
}

TEST_CASE("bound tree pose checkpoints preserve every local channel and reject changed rosters",
          "[rollback][player-pose-checkpoint][tree-pose-checkpoint]") {
    const auto tree = poseTree();
    TreePose original;
    original.evaluate(tree, 4, 3.5f);
    NodePose changed = original.poses()[1];
    changed.rotation = {-0.9f, 1.1f, -0.7f};
    changed.position = {0.4f, -0.3f, 0.2f};
    changed.scale = {-1, 0.8f, 1.2f};
    changed.pitchYawRoll = true;
    original.setNodePose(1, changed);
    const auto state = original.checkpoint();
    REQUIRE(state);
    REQUIRE(state->valid());
    TreePose restored;
    REQUIRE(restored.restore(tree, *state));
    samePose(restored, original);
    CHECK(restored.checkpoint() == state);

    const auto reject = [&](const TreeInfo& trusted, const TreePose::Checkpoint& invalid) {
        CHECK_FALSE(restored.restore(trusted, invalid));
        CHECK(restored.checkpoint() == state);
        samePose(restored, original);
    };
    auto other = tree;
    other.name = "ANOTHER";
    reject(other, *state);
    other = tree;
    other.nodes[1].name = "WRONG_NODE";
    reject(other, *state);
    other = tree;
    other.nodes[2].parent = 0;
    reject(other, *state);
    other = tree;
    other.nodes[0].position.x += 0.01f;
    reject(other, *state);
    other = tree;
    std::swap(other.nodes[1], other.nodes[2]);
    reject(other, *state);
    auto invalid = *state;
    invalid.nodes.pop_back();
    reject(tree, invalid);
    invalid = *state;
    invalid.poses[1].position.y = std::numeric_limits<f32>::quiet_NaN();
    reject(tree, invalid);
    invalid = *state;
    invalid.nodes[1].parent = 1;
    reject(tree, invalid);
    invalid = *state;
    invalid.tree.assign(TreePose::Checkpoint::kMaxName + 1, 'a');
    reject(tree, invalid);
    invalid = *state;
    invalid.nodes[1].name.push_back('\0');
    reject(tree, invalid);
    invalid = *state;
    for (auto& pose : invalid.poses) {
        pose.scale = Vec3{TreePose::Checkpoint::kMaxMagnitude};
    }
    REQUIRE(invalid.valid());
    reject(tree, invalid); // finite local scales may overflow after parent composition
}

TEST_CASE("transition pose wire rejects truncation forged counts flags and nonfinite channels",
          "[netplay][rollback][player-pose-checkpoint]") {
    const auto tree = poseTree();
    PlayerAnimator player;
    REQUIRE(player.bind(tree, false));
    reachBlend(player);
    const auto state = player.transitionPose();
    REQUIRE(state);
    const auto encoded = PlayerPoseCheckpoint::encode(*state);
    REQUIRE(encoded);
    const auto& bytes = *encoded;
    CHECK(bytes.size() == PlayerPoseCheckpoint::kHeaderBytes + state->current.tree.size() +
                              tree.nodes.size() * (PlayerPoseCheckpoint::kNodeBytes + 5));
    for (usize size = 0; size < bytes.size(); ++size) {
        CHECK_FALSE(PlayerPoseCheckpoint::decode(std::span{bytes}.first(size)));
    }
    auto trailing = bytes;
    trailing.push_back(0);
    CHECK_FALSE(PlayerPoseCheckpoint::decode(trailing));
    for (const usize offset : {0U, 4U, 12U}) {
        auto malformed = bytes;
        word(malformed, offset, 0xFFFFFFFF);
        CHECK_FALSE(PlayerPoseCheckpoint::decode(malformed));
    }
    // Header 8, tree 4, first node name length 4, name 5, parent 4, rest 12.
    constexpr usize kFirstPose = 37;
    for (const usize offset : {kFirstPose, kFirstPose + 40}) {
        auto malformed = bytes;
        word(malformed, offset, std::bit_cast<u32>(std::numeric_limits<f32>::infinity()));
        CHECK_FALSE(PlayerPoseCheckpoint::decode(malformed));
        malformed = bytes;
        word(malformed, offset + 36, 2);
        CHECK_FALSE(PlayerPoseCheckpoint::decode(malformed));
    }
    auto invalid = *state;
    invalid.previous.nodes[0].rest.x += 0.1f;
    CHECK_FALSE(PlayerPoseCheckpoint::encode(invalid));
    invalid = *state;
    invalid.current.nodes.resize(TreePose::Checkpoint::kMaxNodes + 1);
    CHECK_FALSE(PlayerPoseCheckpoint::encode(invalid));

    const auto beforeClock = player.checkpoint();
    const auto beforePose = player.transitionPose();
    CHECK_FALSE(player.restore(beforeClock, std::nullopt));
    CHECK(player.checkpoint() == beforeClock);
    CHECK(player.transitionPose() == beforePose);
    invalid = *state;
    invalid.current.nodes[1].name = "FOREIGN";
    invalid.previous.nodes[1].name = "FOREIGN";
    REQUIRE(invalid.valid());
    CHECK_FALSE(player.restore(beforeClock, invalid));
    CHECK(player.checkpoint() == beforeClock);
    CHECK(player.transitionPose() == beforePose);
    auto badClock = beforeClock;
    badClock.animation.sequence = std::numeric_limits<u32>::max();
    CHECK_FALSE(player.restore(badClock, beforePose));
    CHECK(player.checkpoint() == beforeClock);
    CHECK(player.transitionPose() == beforePose);
    PlayerAnimator idle;
    REQUIRE(idle.bind(tree, false));
    CHECK_FALSE(idle.restore(idle.checkpoint(), beforePose));
    CHECK_FALSE(idle.transitionPose());
}

TEST_CASE("exact player pose restore survives interrupted blends and native early update branches",
          "[netplay][rollback][player-pose-checkpoint]") {
    const auto tree = poseTree();
    PlayerAnimator player;
    REQUIRE(player.bind(tree, false));
    player.setCharacter(0);
    constexpr std::array kInterrupts{PlayerDeed::None,         PlayerDeed::Attack,
                                     PlayerDeed::StrongAttack, PlayerDeed::Defend,
                                     PlayerDeed::Flinch,       PlayerDeed::TurboStrong,
                                     PlayerDeed::TurboFull,    PlayerDeed::ComboHeld};
    for (s32 round = 0; round < 3; ++round) {
        reachBlend(player);
        const PlayerAnimator baseline = player;
        for (const auto deed : kInterrupts) {
            CAPTURE(round, static_cast<u32>(deed));
            PlayerAnimator native = baseline;
            PlayerAnimator replay;
            REQUIRE(replay.bind(tree, false));
            REQUIRE(replay.restore(transportedClock(native), transported(native)));
            sameAnimator(replay, native);
            for (s32 tick = 0; tick < 12; ++tick) {
                const auto request = tick == 0 ? deed : PlayerDeed::None;
                native.update(PlayerMotion::Stand, 1, kStep, request);
                replay.update(PlayerMotion::Stand, 1, kStep, request);
                sameAnimator(replay, native);
                // Reconcile again while the old blend is still live, including
                // the ComboHeld early branch which may show an unblended pose.
                REQUIRE(replay.restore(transportedClock(native), transported(native)));
                sameAnimator(replay, native);
            }
        }
        player.update(PlayerMotion::Run, 1, kStep);
    }
}

std::pair<PlayerMotion, PlayerDeed> input(s32 tick) {
    const s32 phase = tick % 180;
    if (phase < 35) {
        return {PlayerMotion::Run, PlayerDeed::None};
    }
    if (phase == 95) {
        return {PlayerMotion::Stand, PlayerDeed::Attack};
    }
    if (phase == 115) {
        return {PlayerMotion::Stand, PlayerDeed::Defend};
    }
    if (phase == 135) {
        return {PlayerMotion::Stand, PlayerDeed::Flinch};
    }
    return {PlayerMotion::Stand, PlayerDeed::None};
}

TEST_CASE("native class and Sumner costume poses survive wire correction and replay",
          "[netplay][rollback][player-pose-checkpoint][assets]") {
    const s32 character = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
    CAPTURE(character);
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    CharacterSave save;
    save.character = character;
    if (character == kSumnerClass) {
        // Sumner's playable artwork is the WIZ/SUM name-code costume,
        // not a SUM/YEL class-colour archive.
        save.name = "SUM224";
        REQUIRE(applyNameCheats(save));
    }
    auto figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(figure);
    REQUIRE(figure->actionTree());
    const auto& tree = *figure->actionTree();
    usize blends = 0;
    usize transitions = 0;
    u64 lastTransition = 0;
    for (s32 tick = 0; tick < 360; ++tick) {
        CAPTURE(tick);
        const auto [motion, deed] = input(tick);
        figure->animate(motion == PlayerMotion::Run ? 1.0f : 0.0f, 1, kStep, deed);
        const auto& live = figure->animator();
        const auto transition = transported(live);
        if (transition) {
            ++blends;
            if (lastTransition != live.player().generation()) {
                ++transitions;
                lastTransition = live.player().generation();
            }
        }
        PlayerAnimator replay;
        REQUIRE(replay.bind(tree, false));
        REQUIRE(replay.restore(transportedClock(live), transition));
        sameAnimator(replay, live);
        const auto placement =
            glm::translate(glm::rotate(Mat4{1}, 0.43f, Vec3{0, 1, 0}), Vec3{2.1f, 1.7f, -4.2f});
        CHECK(figure->rootAttachment(placement, replay.pose()) ==
              figure->rootAttachment(placement));
        for (const auto* const suffix : {"HEAD", "L_HAND", "R_HAND"}) {
            CHECK(figure->attachment(placement, suffix, replay.pose()) ==
                  figure->attachment(placement, suffix));
        }
        PlayerAnimator expected = live;
        for (s32 lead = 1; lead <= 5; ++lead) {
            auto [nextMotion, nextDeed] = input(tick + lead);
            if (lead == 2 && tick % 47 == 0) {
                nextDeed = PlayerDeed::TurboStrong;
            }
            expected.update(nextMotion, 1, kStep, nextDeed);
            replay.update(nextMotion, 1, kStep, nextDeed);
            sameAnimator(replay, expected);
        }
    }
    CHECK(blends > 0);
    CHECK(transitions >= 2);
}
} // namespace
