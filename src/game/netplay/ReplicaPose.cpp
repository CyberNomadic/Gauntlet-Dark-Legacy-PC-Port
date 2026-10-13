#include "game/netplay/ReplicaPose.h"

#include <utility>

namespace gdl::game {
bool ReplicaPose::accepts(const TreeInfo* tree, const CombatAnimation& animation) {
    if (tree == nullptr || !animation.valid() || animation.generation == 0 ||
        animation.sequence >= tree->sequences.size()) {
        return false;
    }
    const auto frames = tree->sequences[animation.sequence].frames;
    // Some native player release entries (ARC THROW1) have zero duration.
    // The host still evaluates their frame-zero tracks during the transition;
    // these are not missing sequences and must not disconnect a remote view.
    return frames >= 0 && animation.frame <= static_cast<f32>(frames > 0 ? frames - 1 : 0);
}
void ReplicaPose::bind(const TreeInfo* tree) {
    m_tree = tree;
    m_generation = 0;
    m_pose = {};
    m_from = {};
}
bool ReplicaPose::rest() {
    m_generation = 0;
    m_from = {};
    if (m_tree == nullptr) {
        m_pose = {};
        return false;
    }
    m_pose.rest(*m_tree);
    return true;
}
bool ReplicaPose::show(const CombatAnimation& animation) {
    if (!accepts(m_tree, animation)) {
        m_pose = {};
        m_from = {};
        m_generation = 0;
        return false;
    }
    if (m_generation != animation.generation) {
        m_from = m_pose;
        m_generation = animation.generation;
    }
    m_pose.evaluate(*m_tree, animation.sequence, animation.frame, false, true);
    if (animation.transition < 1 && m_from.posed()) {
        m_pose.blend(m_from, animation.transition);
    }
    return true;
}
bool ReplicaPose::show(const CombatAnimation& animation,
                       const PlayerAnimator::TransitionPose& transition) {
    if (!accepts(m_tree, animation) || animation.transition >= 1 || !transition.valid()) {
        return false;
    }
    TreePose current;
    TreePose previous;
    if (!current.restore(*m_tree, transition.current) ||
        !previous.restore(*m_tree, transition.previous)) {
        return false;
    }
    // The checkpoint carries the exact interrupted stance, but the displayed
    // cursor can lie between host ticks. Re-sample its authored destination and
    // blend from that stance, not from the last pose this renderer happened to
    // draw. Copying checkpoint.current here would freeze every fractional render.
    current.evaluate(*m_tree, animation.sequence, animation.frame, false, true);
    current.blend(previous, animation.transition);
    m_pose = std::move(current);
    m_from = std::move(previous);
    m_generation = animation.generation;
    return true;
}
} // namespace gdl::game
