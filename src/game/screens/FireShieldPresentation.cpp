#include "game/screens/FireShieldPresentation.h"

#include <algorithm>
#include <cmath>

#include "game/world/PowerupCompanion.h"

namespace gdl::game {
bool FireShieldPresentation::restore(const TreeInfo& tree,
                                     const std::optional<CompanionState>& state) {
    if (tree.name != PowerupCompanion::treeOf(PowerupCompanion::Kind::FireShield) ||
        tree.sequences.size() != 1 || tree.sequences.front().frames <= 0 ||
        std::ranges::any_of(tree.nodes, [](const auto& node) { return node.particle >= 0; })) {
        return false;
    }
    AnimationPlayer animation;
    TextureAnimator textures;
    if (state) {
        if (!state->valid(1) ||
            state->form != static_cast<u32>(PowerupCompanion::Kind::FireShield) ||
            !state->rollback) {
            return false;
        }
        const auto& seed = *state->rollback;
        const auto& sequence = tree.sequences.front();
        const f32 period = (sequence.frameRate > 0 ? static_cast<f32>(sequence.frameRate)
                                                   : AnimationPlayer::kDefaultRate) *
                           AnimationPlayer::kRateUnit;
        if (seed.sequence != 0 || !seed.playing || seed.held || seed.smooth || seed.speed != 1 ||
            seed.secondsPerFrame != period || seed.transitionLength != 0 ||
            seed.time >= static_cast<f64>(sequence.frames) * period ||
            seed.frame > static_cast<f32>(sequence.frames - 1) ||
            !animation.restore(seed, &sequence)) {
            return false;
        }
        const auto whole = static_cast<u32>(state->textureClock);
        textures.seek(whole);
        textures.advance((state->textureClock - static_cast<f32>(whole)) / 30.0f);
    }
    m_tree = &tree;
    m_animation = animation;
    m_textures = std::move(textures);
    m_shown = state.has_value();
    m_alpha = state ? state->alpha : 1;
    return true;
}

bool FireShieldPresentation::step(bool preStepRunning, f32 seconds, f32 alpha) {
    if (m_tree == nullptr || !std::isfinite(seconds) || seconds < 0 || seconds > 0.25f ||
        !std::isfinite(alpha) || alpha < 0 || alpha > 1) {
        return false;
    }
    if (preStepRunning != m_shown) {
        m_animation.stop();
        m_textures.clear();
        m_shown = preStepRunning;
        if (m_shown) {
            m_animation.start(m_tree->sequences.front(), 0);
        }
    }
    m_alpha = alpha;
    if (m_shown) {
        m_animation.advance(seconds, true);
        m_textures.advance(seconds);
    }
    return true;
}

std::optional<CompanionState> FireShieldPresentation::sample(const Mat4& body) const {
    if (!m_shown) {
        return std::nullopt;
    }
    return CompanionState{
        static_cast<u32>(PowerupCompanion::Kind::FireShield),
        body,
        {0, m_animation.sequence(), m_animation.generation(), m_animation.presentationFrame(), 1},
        static_cast<f32>(m_textures.frame()) + m_textures.presentationOffset(1).value_or(0),
        m_alpha,
        m_animation.checkpoint()};
}
} // namespace gdl::game
