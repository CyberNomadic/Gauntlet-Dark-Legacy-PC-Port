#include "game/world/ItemFigureCheckpoint.h"

#include <cmath>
#include <limits>
#include <utility>

namespace gdl::game {
namespace {
bool scalar(f32 value) {
    return std::isfinite(value) && std::abs(value) <= TreePose::Checkpoint::kMaxMagnitude;
}
bool vector(const Vec3& value) {
    return scalar(value.x) && scalar(value.y) && scalar(value.z);
}
bool matrix(const Mat4& value) {
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            if (!scalar(value[column][row])) {
                return false;
            }
        }
    }
    return value[0].w == 0 && value[1].w == 0 && value[2].w == 0 && value[3].w == 1;
}
} // namespace

bool ItemFigureCheckpoint::State::valid() const {
    if (bound) {
        return archive && state.valid();
    }
    constexpr auto kLastGeneration =
        std::numeric_limits<u64>::max() - AnimationPlayer::kCheckpointGenerationHeadroom;
    return state.pose == TreePose::Checkpoint{} && !state.presentationPose &&
           state.particles == TreeParticles{}.checkpoint() && state.textures.valid() &&
           state.textures.frame == 0 && state.textures.remainder == 0 && state.animation.valid() &&
           !state.animation.playing && state.poseSequence == 0 && state.poseFrame == 0 &&
           state.poseGeneration == 0 && matrix(state.previousTransform) &&
           scalar(state.previousFrame) && state.previousFrame >= 0 &&
           state.previousGeneration <= kLastGeneration && state.textureSequence == 0 &&
           state.textureFrame == 0 && vector(state.position) && scalar(state.yaw) &&
           matrix(state.transform) && matrix(state.placement) && state.index >= -1 &&
           state.index <= static_cast<s32>(TreePose::Checkpoint::kMaxMagnitude);
}

std::optional<ItemFigureCheckpoint::State> ItemFigureCheckpoint::capture(const ItemFigure& figure) {
    if (figure.hasFigure()) {
        const auto state = figure.checkpoint();
        return state ? std::optional{State{*state, true, true}} : std::nullopt;
    }
    if (figure.m_staticTree || figure.m_model.bound() || figure.m_pose.posed() ||
        figure.m_presentationPose.posed() || figure.m_particles.field().size() != 0 ||
        figure.m_textures.size() != 0) {
        return std::nullopt;
    }
    State result;
    result.archive = figure.m_archive != nullptr;
    auto& state = result.state;
    state.animation = figure.m_player.checkpoint();
    state.textures = figure.m_textures.checkpoint();
    state.particles = figure.m_particles.checkpoint();
    state.poseSequence = figure.m_poseSequence;
    state.poseFrame = figure.m_poseFrame;
    state.poseGeneration = figure.m_poseGeneration;
    state.continuity = figure.m_continuity;
    state.previousTransform = figure.m_previousTransform;
    state.previousFrame = figure.m_previousFrame;
    state.previousGeneration = figure.m_previousGeneration;
    state.presentationCaptured = figure.m_presentationCaptured;
    state.presentationAdvanced = figure.m_presentationAdvanced;
    state.textureSequence = figure.m_textureSequence;
    state.textureFrame = figure.m_textureFrame;
    state.gateParticles = figure.m_gateParticles;
    state.position = figure.m_position;
    state.yaw = figure.m_yaw;
    state.transform = figure.m_transform;
    state.placement = figure.m_placement;
    state.index = figure.m_index;
    state.loop = figure.m_loop;
    state.holdPose = figure.m_holdPose;
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}

std::unique_ptr<ItemFigure> ItemFigureCheckpoint::prepare(const ItemFigure& figure,
                                                          const State& saved) {
    if (!saved.valid() || saved.bound != figure.hasFigure() ||
        saved.archive != (figure.m_archive != nullptr)) {
        return nullptr;
    }
    if (saved.bound) {
        return figure.restored(saved.state);
    }
    if (!capture(figure)) {
        return nullptr;
    }
    auto result = std::make_unique<ItemFigure>();
    const auto& state = saved.state;
    if (!result->m_player.restore(state.animation, nullptr) ||
        !result->m_textures.restore(state.textures) ||
        !result->m_particles.restore(state.particles)) {
        return nullptr;
    }
    result->m_archive = figure.m_archive;
    result->m_poseSequence = state.poseSequence;
    result->m_poseFrame = state.poseFrame;
    result->m_poseGeneration = state.poseGeneration;
    result->m_continuity = state.continuity;
    result->m_previousTransform = state.previousTransform;
    result->m_previousFrame = state.previousFrame;
    result->m_previousGeneration = state.previousGeneration;
    result->m_presentationCaptured = state.presentationCaptured;
    result->m_presentationAdvanced = state.presentationAdvanced;
    result->m_textureSequence = state.textureSequence;
    result->m_textureFrame = state.textureFrame;
    result->m_gateParticles = state.gateParticles;
    result->m_position = state.position;
    result->m_yaw = state.yaw;
    result->m_transform = state.transform;
    result->m_placement = state.placement;
    result->m_index = state.index;
    result->m_loop = state.loop;
    result->m_holdPose = state.holdPose;
    return result;
}
} // namespace gdl::game
