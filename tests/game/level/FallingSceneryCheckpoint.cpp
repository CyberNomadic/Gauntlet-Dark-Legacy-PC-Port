#include "game/level/FallingSceneryCheckpoint.h"

#include <limits>
#include <utility>

namespace gdl::game {
namespace fc = fixture_checkpoint;

bool FallingSceneryCheckpoint::State::valid() const {
    if (pieces.size() > kMaxPieces || (!bound && !pieces.empty()) || !fc::name(breakSound) ||
        !fc::name(leafSound) || !fc::scalar(remainder) || remainder < -0.000002f ||
        remainder >= FallingPiece::kStep || !fc::scalar(updateSeconds) || updateSeconds < 0 ||
        !fc::scalar(bottom)) {
        return false;
    }
    usize details = 0;
    for (usize index = 0; index < pieces.size(); ++index) {
        const auto& p = pieces[index];
        const auto& info = p.source.info;
        const auto& source = p.source.instance;
        const s32 override =
            static_cast<s32>(source.params[0]) | (static_cast<s32>(source.params[1]) << 8);
        const s32 subtype = override > 0 ? override : info.subtype;
        const auto profile = FallingProfile::of(subtype);
        if (!p.source.valid() || (index != 0 && p.source.index <= pieces[index - 1].source.index) ||
            info.type != FallingScenery::kObstacle || subtype != p.subtype ||
            (subtype != FallingScenery::kFallAway && subtype != FallingScenery::kLeafFall &&
             subtype != FallingScenery::kShootFall && subtype != FallingScenery::kRockSink) ||
            p.gravityStep != profile.gravityStep || p.spin != profile.spin ||
            p.radius != info.radius || p.height != info.height ||
            p.centre != info.collisionOffset || p.minPlayers != source.minPlayers ||
            !fc::vector(p.position) || !fc::vector(p.rotation) || !fc::vector(p.velocity) ||
            !fc::vector(p.previousPosition) || !fc::vector(p.previousRotation)) {
            return false;
        }
        if (info.choices.size() > kMaxDetails - details) {
            return false;
        }
        details += info.choices.size();
        if (source.collision.size() > kMaxDetails - details) {
            return false;
        }
        details += source.collision.size();
    }
    return true;
}

struct FallingSceneryCheckpoint::Prepared::Data {
    const FallingSceneryCheckpoint* wrapper = nullptr;
    const FallingScenery* owner = nullptr;
    const WorldLayout* layout = nullptr;
    const ModelSet* models = nullptr;
    const TextureSet* textures = nullptr;
    std::shared_ptr<const u8> lifetime, binding;
    u64 generation = 0;
    State state;
    const FallingScenery::Piece* base = nullptr;
};
FallingSceneryCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data)
    : m_data(std::move(data)) {}
FallingSceneryCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
FallingSceneryCheckpoint::Prepared&
FallingSceneryCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
FallingSceneryCheckpoint::Prepared::~Prepared() = default;

std::optional<FallingSceneryCheckpoint::State> FallingSceneryCheckpoint::checkpoint() const {
    State result;
    result.breakSound = m_owner.m_breakSound;
    result.leafSound = m_owner.m_leafSound;
    result.remainder = m_owner.m_clock.remainder;
    result.updateSeconds = m_owner.m_clock.updateSeconds;
    result.bottom = m_owner.m_bottom;
    result.bound = m_owner.m_layout != nullptr;
    for (const auto& piece : m_owner.m_pieces) {
        const auto source = fc::placement(m_owner.m_layout, piece.motion.instance);
        if (!source || !piece.model.bound()) {
            return std::nullopt;
        }
        result.pieces.push_back(
            {*source, piece.motion.position, piece.motion.rotation, piece.motion.velocity,
             piece.motion.m_previousPosition, piece.motion.m_previousRotation, piece.motion.visible,
             piece.profile.gravityStep, piece.profile.spin, piece.subtype, piece.minPlayers,
             piece.radius, piece.height, piece.centre, piece.shown, piece.started});
    }
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}
std::optional<FallingSceneryCheckpoint::Prepared>
FallingSceneryCheckpoint::prepareRestore(const State& state) const {
    if (!state.valid() || state.pieces.size() != m_owner.m_pieces.size() ||
        state.bound != (m_owner.m_layout != nullptr) || state.breakSound != m_owner.m_breakSound ||
        state.leafSound != m_owner.m_leafSound || state.bottom != m_owner.m_bottom ||
        m_owner.m_checkpointGeneration == std::numeric_limits<u64>::max()) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->wrapper = this;
    data->owner = &m_owner;
    data->layout = m_owner.m_layout;
    data->models = m_owner.m_models;
    data->textures = m_owner.m_textures;
    data->lifetime = m_lifetime;
    data->binding = m_owner.m_checkpointBinding;
    data->generation = m_owner.m_checkpointGeneration;
    data->state = state;
    data->base = m_owner.m_pieces.data();
    Prepared result{std::move(data)};
    return canCommit(result) ? std::optional{std::move(result)} : std::nullopt;
}
bool FallingSceneryCheckpoint::canCommit(const Prepared& prepared) const {
    if (!prepared.m_data) {
        return false;
    }
    const auto& data = *prepared.m_data;
    if (data.wrapper != this || data.owner != &m_owner || data.lifetime != m_lifetime ||
        data.binding != m_owner.m_checkpointBinding ||
        data.generation != m_owner.m_checkpointGeneration || data.layout != m_owner.m_layout ||
        data.models != m_owner.m_models || data.textures != m_owner.m_textures ||
        data.base != m_owner.m_pieces.data() ||
        data.state.pieces.size() != m_owner.m_pieces.size()) {
        return false;
    }
    for (usize index = 0; index < data.state.pieces.size(); ++index) {
        const auto& saved = data.state.pieces[index];
        const auto& live = m_owner.m_pieces[index];
        if ((data.layout == nullptr) || !saved.source.matches(*data.layout) ||
            saved.source.index != live.motion.instance || (data.models == nullptr) ||
            !data.models->find(saved.source.instance.name) || !live.model.bound() ||
            saved.subtype != live.subtype || saved.gravityStep != live.profile.gravityStep ||
            saved.spin != live.profile.spin || saved.radius != live.radius ||
            saved.height != live.height || saved.centre != live.centre ||
            saved.minPlayers != live.minPlayers) {
            return false;
        }
    }
    return true;
}
bool FallingSceneryCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    for (usize index = 0; index < data->state.pieces.size(); ++index) {
        const auto& saved = data->state.pieces[index];
        auto& live = m_owner.m_pieces[index];
        live.motion.position = saved.position;
        live.motion.rotation = saved.rotation;
        live.motion.velocity = saved.velocity;
        live.motion.m_previousPosition = saved.previousPosition;
        live.motion.m_previousRotation = saved.previousRotation;
        live.motion.visible = saved.visible;
        live.shown = saved.shown;
        live.started = saved.started;
    }
    m_owner.m_clock.remainder = data->state.remainder;
    m_owner.m_clock.updateSeconds = data->state.updateSeconds;
    ++m_owner.m_checkpointGeneration;
    return true;
}
bool FallingSceneryCheckpoint::restore(const State& state) {
    auto plan = prepareRestore(state);
    return plan && commitRestore(std::move(*plan));
}
} // namespace gdl::game
