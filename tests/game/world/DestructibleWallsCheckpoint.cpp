#include "game/world/DestructibleWallsCheckpoint.h"

#include <limits>
#include <tuple>
#include <utility>

namespace gdl::game {
namespace fc = fixture_checkpoint;
namespace {
DestructibleWallsCheckpoint::Bounds boundsOf(const MissileTarget& target) {
    DestructibleWallsCheckpoint::Bounds result{target.id,
                                               target.base,
                                               target.radius,
                                               target.height,
                                               target.node,
                                               target.targetScoreScale,
                                               target.maxTargetDistance,
                                               target.potionBottle,
                                               std::nullopt};
    if (target.acquisition) {
        const auto& value = *target.acquisition;
        result.acquisition = DestructibleWallsCheckpoint::Acquisition{
            value.point, value.radius, value.distanceScale, value.maxHeight, value.enabled};
    }
    return result;
}
auto values(const DestructibleWallsCheckpoint::Wall& value) {
    return std::tie(value.source, value.transform, value.collisionCentre, value.bounds,
                    value.health, value.armor, value.minPlayers, value.object, value.shown,
                    value.flash, value.blinks, value.blinkTicks, value.white, value.model);
}
bool sameImmutable(const DestructibleWallsCheckpoint::Wall& saved,
                   const DestructibleWalls::Wall& live) {
    return saved.transform == live.transform && saved.collisionCentre == live.collisionCentre &&
           fc::equal(saved.surface, live.surface) && saved.bounds == boundsOf(live.bounds) &&
           live.bounds.surface.empty() && saved.armor == live.armor &&
           saved.minPlayers == live.minPlayers && saved.object == live.object &&
           saved.model == live.model.bound();
}
} // namespace

bool DestructibleWallsCheckpoint::Bounds::valid() const {
    return id >= -1 && id <= 65535 && fc::vector(base) && fc::scalar(radius) && radius >= 0 &&
           fc::scalar(height) && height >= 0 && node >= -1 && node <= 65535 &&
           fc::scalar(targetScoreScale) && targetScoreScale >= 0 && fc::scalar(maxTargetDistance) &&
           maxTargetDistance >= 0 &&
           (!acquisition || (fc::vector(acquisition->point) && fc::scalar(acquisition->radius) &&
                             acquisition->radius >= 0 && fc::scalar(acquisition->distanceScale) &&
                             acquisition->distanceScale >= 0 &&
                             fc::scalar(acquisition->maxHeight) && acquisition->maxHeight >= 0));
}
bool DestructibleWallsCheckpoint::Wall::operator==(const Wall& other) const {
    return values(*this) == values(other) && fc::equal(surface, other.surface);
}
bool DestructibleWallsCheckpoint::State::valid() const {
    if (walls.size() > kMaxWalls || (!bound && !walls.empty())) {
        return false;
    }
    usize details = 0;
    for (usize index = 0; index < walls.size(); ++index) {
        const auto& wall = walls[index];
        const auto& info = wall.source.info;
        const auto& instance = wall.source.instance;
        const s32 override =
            static_cast<s32>(instance.params[0]) | (static_cast<s32>(instance.params[1]) << 8);
        if (!wall.source.valid() ||
            (index != 0 && wall.source.index <= walls[index - 1].source.index) || info.type != 10 ||
            (override > 0 ? override : info.subtype) != 42 || !fc::matrix(wall.transform) ||
            !fc::vector(wall.collisionCentre) || !wall.bounds.valid() ||
            wall.health < std::min(info.hitPoints, 0) ||
            wall.health > std::max(info.hitPoints, 0) || wall.armor != info.armor ||
            wall.minPlayers != instance.minPlayers || wall.object < 0 || wall.object > 131070 ||
            !fc::scalar(wall.flash) || wall.flash < 0 || wall.flash > 1.0f / 30 ||
            wall.blinks < 0 || wall.blinks > DestructibleWalls::kRevealBlinks ||
            !fc::scalar(wall.blinkTicks) || wall.blinkTicks < 0 ||
            wall.blinkTicks > DestructibleWalls::kBlinkCycle || (wall.white && !whiteTexture)) {
            return false;
        }
        for (const usize count :
             {info.choices.size(), instance.collision.size(), wall.surface.size()}) {
            if (count > kMaxDetails - details) {
                return false;
            }
            details += count;
        }
        for (const auto& triangle : wall.surface) {
            if (triangle.object != wall.object || !fc::vector(triangle.normal) ||
                !std::ranges::all_of(triangle.vertices, fc::vector)) {
                return false;
            }
        }
    }
    return true;
}

struct DestructibleWallsCheckpoint::Prepared::Data {
    const DestructibleWallsCheckpoint* wrapper = nullptr;
    const DestructibleWalls* owner = nullptr;
    const WorldLayout* layout = nullptr;
    const ModelSet* models = nullptr;
    const TextureSet* textures = nullptr;
    const WorldCollision* collision = nullptr;
    const Texture* white = nullptr;
    std::shared_ptr<const u8> lifetime, binding;
    u64 generation = 0;
    State state;
    const DestructibleWalls::Wall* base = nullptr;
    std::vector<const CollisionTriangle*> surfaces;
    std::vector<TreeModel> stagedModels;
};
DestructibleWallsCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data)
    : m_data(std::move(data)) {}
DestructibleWallsCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
DestructibleWallsCheckpoint::Prepared&
DestructibleWallsCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
DestructibleWallsCheckpoint::Prepared::~Prepared() = default;

std::optional<DestructibleWallsCheckpoint::State> DestructibleWallsCheckpoint::checkpoint() const {
    State result;
    result.bound = m_owner.m_layout != nullptr;
    result.whiteTexture = m_owner.m_white != nullptr;
    for (const auto& wall : m_owner.m_walls) {
        if (!m_owner.m_layout || wall.object < 0 ||
            static_cast<usize>(wall.object) < m_owner.m_layout->objects().size() ||
            !wall.bounds.surface.empty()) {
            return std::nullopt;
        }
        const auto source = fc::placement(m_owner.m_layout, static_cast<usize>(wall.object) -
                                                                m_owner.m_layout->objects().size());
        if (!source) {
            return std::nullopt;
        }
        result.walls.push_back({*source, wall.transform, wall.collisionCentre, wall.surface,
                                boundsOf(wall.bounds), wall.health, wall.armor, wall.minPlayers,
                                wall.object, wall.shown, wall.flash, wall.blinks, wall.blinkTicks,
                                wall.white, wall.model.bound()});
    }
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}
std::optional<DestructibleWallsCheckpoint::Prepared>
DestructibleWallsCheckpoint::prepareRestore(const State& state) const {
    if (!state.valid() || state.walls.size() != m_owner.m_walls.size() ||
        state.bound != (m_owner.m_layout != nullptr) ||
        state.whiteTexture != (m_owner.m_white != nullptr) ||
        m_owner.m_checkpointGeneration == std::numeric_limits<u64>::max()) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->wrapper = this;
    data->owner = &m_owner;
    data->layout = m_owner.m_layout;
    data->models = m_owner.m_models;
    data->textures = m_owner.m_textures;
    data->collision = m_owner.m_collision;
    data->white = m_owner.m_white;
    data->lifetime = m_lifetime;
    data->binding = m_owner.m_checkpointBinding;
    data->generation = m_owner.m_checkpointGeneration;
    data->state = state;
    data->base = m_owner.m_walls.data();
    data->stagedModels.reserve(state.walls.size());
    for (usize index = 0; index < state.walls.size(); ++index) {
        const auto& saved = state.walls[index];
        const auto& live = m_owner.m_walls[index];
        if (!sameImmutable(saved, live)) {
            return std::nullopt;
        }
        data->surfaces.push_back(live.surface.data());
        auto& model = data->stagedModels.emplace_back(live.model);
        model.setMaskedTexture(saved.white ? m_owner.m_white : nullptr);
        model.setAppearance(saved.white);
    }
    Prepared result{std::move(data)};
    return canCommit(result) ? std::optional{std::move(result)} : std::nullopt;
}
bool DestructibleWallsCheckpoint::canCommit(const Prepared& prepared) const {
    if (!prepared.m_data) {
        return false;
    }
    const auto& data = *prepared.m_data;
    if (data.wrapper != this || data.owner != &m_owner || data.lifetime != m_lifetime ||
        data.binding != m_owner.m_checkpointBinding ||
        data.generation != m_owner.m_checkpointGeneration || data.layout != m_owner.m_layout ||
        data.models != m_owner.m_models || data.textures != m_owner.m_textures ||
        data.collision != m_owner.m_collision || data.white != m_owner.m_white ||
        data.base != m_owner.m_walls.data() || data.state.walls.size() != m_owner.m_walls.size()) {
        return false;
    }
    for (usize index = 0; index < data.state.walls.size(); ++index) {
        const auto& saved = data.state.walls[index];
        const auto& live = m_owner.m_walls[index];
        if (!data.layout || !saved.source.matches(*data.layout) ||
            saved.source.index + data.layout->objects().size() != static_cast<usize>(live.object) ||
            data.surfaces[index] != live.surface.data() || !sameImmutable(saved, live)) {
            return false;
        }
        const auto& object = saved.source.instance.name.empty() ? saved.source.info.name
                                                                : saved.source.instance.name;
        if (saved.model && ((data.models == nullptr) || !data.models->find(object))) {
            return false;
        }
    }
    return true;
}
bool DestructibleWallsCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    for (usize index = 0; index < data->state.walls.size(); ++index) {
        const auto& saved = data->state.walls[index];
        auto& live = m_owner.m_walls[index];
        live.health = saved.health;
        live.shown = saved.shown;
        live.flash = saved.flash;
        live.blinks = saved.blinks;
        live.blinkTicks = saved.blinkTicks;
        live.white = saved.white;
        live.model = std::move(data->stagedModels[index]);
    }
    ++m_owner.m_checkpointGeneration;
    return true;
}
bool DestructibleWallsCheckpoint::restore(const State& state) {
    auto plan = prepareRestore(state);
    return plan && commitRestore(std::move(*plan));
}
} // namespace gdl::game
