#include "game/world/PlayerFamiliar.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "engine/io/NativeStateDigest.h"

namespace gdl::game {
bool PlayerFamiliar::appendState(NativeStateDigest& digest, const State& state) const {
    const auto& [pose, previousPose, player, textures, offset, previousGeneration,
                 previousFrame, presentationAdvanced, tier] = state;
    digest.string("PlayerFamiliar.native.1");
    digest.resource(m_tree);
    // The resolved frame bank is immutable. Its mutable render clock is replaced
    // by the State clock on every update/draw and is not another source of truth.
    m_textures.appendBindingDigest(digest);
    digest.values(pose, previousPose, player);
    digest.fields<3>(textures);
    digest.values(offset, previousGeneration, previousFrame, presentationAdvanced, tier);
    return digest.good();
}

bool PlayerFamiliar::appendNativeDigest(NativeStateDigest& digest) const {
    return appendState(digest, m_state);
}

bool PlayerFamiliar::appendLocalDigest(NativeStateDigest& digest,
                                      const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    const auto& [state, owner, bindingRevision] = saved;
    return appendState(digest, state);
}

void PlayerFamiliar::saveLocal(LocalSnapshot& state) const {
    state.m_state = m_state;
    state.m_owner = m_localOwner;
    state.m_bindingRevision = m_bindingRevision;
}
bool PlayerFamiliar::canRestoreLocal(const LocalSnapshot& state) const {
    return state.m_owner.lock() == m_localOwner && state.m_bindingRevision == m_bindingRevision;
}
bool PlayerFamiliar::restoreLocal(const LocalSnapshot& state) {
    if (!canRestoreLocal(state)) {
        return false;
    }
    m_state = state.m_state;
    m_drawPose = {};
    return true;
}

bool PlayerFamiliar::bind(RenderDevice& device, ItemArchive& archive, s32 level, const Vec3& offset,
                          std::span<TextureSet* const> lenders) {
    m_state.tier = tierFor(level);
    return bindTree(device, archive, m_state.tier > 0 ? std::format("FAMILIAR{}", m_state.tier) : "",
                    offset, lenders);
}
bool PlayerFamiliar::bindTree(RenderDevice& device, ItemArchive& archive, std::string_view name,
                              const Vec3& offset, std::span<TextureSet* const> lenders) {
    ++m_bindingRevision;
    m_tree = nullptr;
    m_model.clear();
    m_textures.clear();
    m_state.textures = {};
    m_state.player.stop();
    m_state.pose = TreePose{};
    m_state.previousPose = TreePose{};
    m_state.offset = offset;
    m_state.presentationAdvanced = false;
    const auto index = archive.trees.find(name);
    if (name.empty() || !index.has_value()) {
        return false;
    }
    const auto& tree = archive.trees.tree(*index);
    if (tree.sequences.empty() ||
        !m_model.bind(tree, archive.models, archive.textures, device, lenders)) {
        return false;
    }
    m_tree = &tree;
    m_textures.bind(archive.trees.textureAnimations(), archive.textures, device, lenders);
    m_state.player.start(tree.sequences[0], 0);
    update(0, false);
    return true;
}
bool PlayerFamiliar::Checkpoint::valid() const {
    if (tier < 0 || tier > 2 || tree.size() > TreePose::Checkpoint::kMaxName ||
        tree.find('\0') != std::string::npos || !animation.valid() || !textures.valid() ||
        (pose && !pose->valid()) || (previousPose && !previousPose->valid()) ||
        !std::isfinite(previousFrame) || previousFrame < 0 ||
        previousFrame > AnimationPlayer::kCheckpointFrames ||
        previousGeneration > animation.generation || tree.empty() == animation.playing ||
        (!tree.empty() && (!pose || pose->tree != tree)) ||
        (previousPose && previousPose->tree != tree) || (tree.empty() && (pose || previousPose))) {
        return false;
    }
    for (s32 axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(offset[axis]) ||
            std::abs(offset[axis]) > TreePose::Checkpoint::kMaxMagnitude) {
            return false;
        }
    }
    return true;
}
std::optional<PlayerFamiliar::Checkpoint> PlayerFamiliar::checkpoint() const {
    Checkpoint state{m_state.tier,
                     m_tree ? m_tree->name : std::string{},
                     m_state.offset,
                     m_state.player.checkpoint(),
                     m_state.textures,
                     m_state.pose.checkpoint(),
                     m_state.previousPose.checkpoint(),
                     m_state.previousGeneration,
                     m_state.previousFrame,
                     m_state.presentationAdvanced};
    if ((m_state.pose.posed() && !state.pose) || (m_state.previousPose.posed() && !state.previousPose) ||
        !state.valid()) {
        return std::nullopt;
    }
    return state;
}
bool PlayerFamiliar::restore(const Checkpoint& state) {
    if (!state.valid() || state.tier != m_state.tier || state.offset != m_state.offset ||
        state.tree != (m_tree ? m_tree->name : std::string{})) {
        return false;
    }
    const TreeSequenceInfo* sequence = nullptr;
    if (state.animation.playing) {
        if (!m_tree || state.animation.sequence >= m_tree->sequences.size()) {
            return false;
        }
        sequence = &m_tree->sequences[state.animation.sequence];
    }
    auto next = *this;
    if (!next.m_state.player.restore(state.animation, sequence) ||
        (state.pose && !next.m_state.pose.restore(*m_tree, *state.pose)) ||
        (state.previousPose && !next.m_state.previousPose.restore(*m_tree, *state.previousPose))) {
        return false;
    }
    if (!state.pose) {
        next.m_state.pose = {};
    }
    if (!state.previousPose) {
        next.m_state.previousPose = {};
    }
    next.m_drawPose = {};
    next.m_state.previousFrame = state.previousFrame;
    next.m_state.previousGeneration = state.previousGeneration;
    next.m_state.presentationAdvanced = state.presentationAdvanced;
    next.m_state.textures = state.textures;
    *this = std::move(next);
    return true;
}
void PlayerFamiliar::update(f32 seconds, bool attack) {
    if (m_tree == nullptr) {
        return;
    }
    m_state.previousPose = m_state.pose;
    m_state.previousGeneration = m_state.player.generation();
    m_state.previousFrame = m_state.player.presentationFrame();
    m_state.presentationAdvanced = seconds > 0;
    if (attack && m_tree->sequences.size() > 1 && m_state.player.sequence() == 0) {
        m_state.player.start(m_tree->sequences[1], 1);
    }
    m_state.player.advance(seconds, m_state.player.sequence() == 0);
    if (m_state.player.finished() && m_state.player.sequence() != 0) {
        m_state.player.start(m_tree->sequences[0], 0);
    }
    const auto frame = static_cast<s32>(m_state.player.frame());
    m_state.pose.evaluate(*m_tree, m_state.player.sequence(), m_state.player.presentationFrame(),
                          false, true);
    m_model.setFrame(m_state.player.sequence(), frame);
    m_textures.restoreClock(m_state.textures);
    m_textures.advance(seconds);
    m_state.textures = m_textures.clock();
    m_textures.apply(m_model, *m_tree, m_state.player.sequence(), frame);
}
std::optional<CompanionVisual> PlayerFamiliar::visual(const Mat4& body, f32 alpha) const {
    if (m_tree == nullptr) {
        return std::nullopt;
    }
    m_textures.restoreClock(m_state.textures);
    return CompanionVisual{m_tree,
                           &m_model,
                           &m_textures,
                           static_cast<u32>(m_state.tier),
                           glm::translate(body, m_state.offset),
                           m_state.player.sequence(),
                           m_state.player.generation(),
                           m_state.player.presentationFrame(),
                           static_cast<f32>(m_textures.frame()) +
                               m_textures.presentationOffset(1).value_or(0),
                           alpha};
}
void PlayerFamiliar::draw(RenderDevice& device, const Mat4& clip, const Mat4& body,
                          const WorldLighting& lighting, f32 alpha, const CameraFrame* camera,
                          f32 renderAlpha, TreeModel::Pass pass) const {
    if (m_tree != nullptr) {
        const f32 blend = renderAlpha < 0 || m_state.presentationAdvanced ? renderAlpha : 1.0f;
        f32 frame = blend >= 0 ? m_state.player.presentationFrame() : m_state.player.frame();
        m_drawPose = m_state.pose;
        if (blend >= 0 && blend < 1.0f && m_state.previousPose.posed() &&
            m_state.previousGeneration == m_state.player.generation()) {
            const f32 amount = std::clamp(blend, 0.0f, 1.0f);
            m_drawPose.blend(m_state.previousPose, amount, true);
            frame = std::lerp(m_state.previousFrame, frame, amount);
        }
        m_model.setPresentationFrame(m_state.player.sequence(), frame);
        m_textures.restoreClock(m_state.textures);
        m_textures.apply(m_model, *m_tree, m_state.player.sequence(), frame,
                         m_textures.presentationOffset(blend));
        m_model.draw(device, clip, glm::translate(body, m_state.offset), lighting,
                     m_drawPose.matrices(), camera, alpha, pass);
    }
}
} // namespace gdl::game
