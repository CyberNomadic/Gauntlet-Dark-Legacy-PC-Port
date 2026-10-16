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
        auto state = figure.checkpoint();
        return state ? std::optional{State{std::move(*state), true, true}} : std::nullopt;
    }
    if (figure.m_sim.resources->staticTree || figure.m_sim.resources->model.bound() || figure.m_sim.pose.posed() ||
        figure.m_presentationPose.posed() || figure.m_sim.particles.field().size() != 0 ||
        figure.textures().size() != 0) {
        return std::nullopt;
    }
    State result;
    result.archive = figure.m_sim.archive != nullptr;
    auto& state = result.state;
    state.animation = figure.m_sim.player.checkpoint();
    state.textures = figure.textures().checkpoint();
    state.particles = figure.m_sim.particles.checkpoint();
    state.poseSequence = figure.m_sim.poseSequence;
    state.poseFrame = figure.m_sim.poseFrame;
    state.poseGeneration = figure.m_sim.poseGeneration;
    state.continuity = figure.m_sim.continuity;
    state.previousTransform = figure.m_sim.previousTransform;
    state.previousFrame = figure.m_sim.previousFrame;
    state.previousGeneration = figure.m_sim.previousGeneration;
    state.presentationCaptured = figure.m_sim.presentationCaptured;
    state.presentationAdvanced = figure.m_sim.presentationAdvanced;
    state.textureSequence = figure.m_sim.textureSequence;
    state.textureFrame = figure.m_sim.textureFrame;
    state.gateParticles = figure.m_sim.gateParticles;
    state.position = figure.m_sim.position;
    state.yaw = figure.m_sim.yaw;
    state.transform = figure.m_sim.transform;
    state.placement = figure.m_sim.placement;
    state.index = figure.m_sim.index;
    state.loop = figure.m_sim.loop;
    state.holdPose = figure.m_sim.holdPose;
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}

std::unique_ptr<ItemFigure> ItemFigureCheckpoint::prepare(const ItemFigure& figure,
                                                          const State& saved) {
    if (!saved.valid() || saved.bound != figure.hasFigure() ||
        saved.archive != (figure.m_sim.archive != nullptr)) {
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
    if (!result->m_sim.player.restore(state.animation, nullptr) ||
        !result->textures().restore(state.textures) ||
        !result->m_sim.particles.restore(state.particles)) {
        return nullptr;
    }
    result->m_sim.textureClock = state.textures;
    result->m_sim.archive = figure.m_sim.archive;
    result->m_sim.poseSequence = state.poseSequence;
    result->m_sim.poseFrame = state.poseFrame;
    result->m_sim.poseGeneration = state.poseGeneration;
    result->m_sim.continuity = state.continuity;
    result->m_sim.previousTransform = state.previousTransform;
    result->m_sim.previousFrame = state.previousFrame;
    result->m_sim.previousGeneration = state.previousGeneration;
    result->m_sim.presentationCaptured = state.presentationCaptured;
    result->m_sim.presentationAdvanced = state.presentationAdvanced;
    result->m_sim.textureSequence = state.textureSequence;
    result->m_sim.textureFrame = state.textureFrame;
    result->m_sim.gateParticles = state.gateParticles;
    result->m_sim.position = state.position;
    result->m_sim.yaw = state.yaw;
    result->m_sim.transform = state.transform;
    result->m_sim.placement = state.placement;
    result->m_sim.index = state.index;
    result->m_sim.loop = state.loop;
    result->m_sim.holdPose = state.holdPose;
    return result;
}
} // namespace gdl::game
