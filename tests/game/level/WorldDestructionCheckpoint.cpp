#include "game/level/WorldDestructionCheckpoint.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace gdl::game {
namespace {
bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
           std::abs(value.x) <= 1'000'000 && std::abs(value.y) <= 1'000'000 &&
           std::abs(value.z) <= 1'000'000;
}
WorldDestructionCheckpoint::Object identity(const WorldObject& value) {
    return {value.name,  value.position, value.flags,  value.objectFlags, value.next,
            value.child, value.parent,   value.radius, value.noCollision};
}
bool same(const WorldDestructionCheckpoint::Object& value, const WorldObject& native) {
    // Do not copy the name here: final preflight and commit must not allocate.
    return value.name == native.name && value.position == native.position &&
           value.flags == native.flags && value.objectFlags == native.objectFlags &&
           value.next == native.next && value.child == native.child &&
           value.parent == native.parent && value.radius == native.radius &&
           value.noCollision == native.noCollision;
}
} // namespace

bool WorldDestructionCheckpoint::State::valid() const {
    if (objects.size() > kMaxObjects || destroyed.size() != objects.size() ||
        controlled.size() > objects.size() || explosions.size() > kMaxExplosions ||
        !std::ranges::all_of(explosions, finite)) {
        return false;
    }
    const auto index = [&](s32 value) {
        return value == -1 || (value >= 0 && static_cast<usize>(value) < objects.size());
    };
    for (const auto& object : objects) {
        if (object.name.size() > 256 || object.name.find('\0') != std::string::npos ||
            !finite(object.position) || !std::isfinite(object.radius) || object.radius < 0 ||
            object.radius > 1'000'000 || !index(object.next) || !index(object.child) ||
            !index(object.parent)) {
            return false;
        }
    }
    for (usize i = 0; i < controlled.size(); ++i) {
        if (controlled[i] >= objects.size() || (i != 0 && controlled[i] <= controlled[i - 1])) {
            return false;
        }
    }
    for (usize i = 0; i < destroyed.size(); ++i) {
        if (destroyed[i] && !std::ranges::binary_search(controlled, i)) {
            return false;
        }
    }
    return true;
}

struct WorldDestructionCheckpoint::Prepared::Data {
    const WorldDestructionCheckpoint* wrapper = nullptr;
    WorldDestruction* owner = nullptr;
    std::shared_ptr<const u8> lifetime;
    std::shared_ptr<const u8> binding;
    u64 generation = 0;
    State state;
};
WorldDestructionCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data)
    : m_data(std::move(data)) {}
WorldDestructionCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
WorldDestructionCheckpoint::Prepared&
WorldDestructionCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
WorldDestructionCheckpoint::Prepared::~Prepared() = default;

bool WorldDestructionCheckpoint::sameRoster(const State& state) const {
    return state.objects.size() == m_owner.m_objects.size() &&
           state.controlled == m_owner.m_controlled &&
           std::ranges::equal(state.objects, m_owner.m_objects, same);
}
std::optional<WorldDestructionCheckpoint::State> WorldDestructionCheckpoint::checkpoint() const {
    State state;
    state.objects.reserve(m_owner.m_objects.size());
    for (const auto& object : m_owner.m_objects) {
        state.objects.push_back(identity(object));
    }
    state.destroyed = m_owner.m_sim.destroyed;
    state.controlled = m_owner.m_controlled;
    state.explosions = m_owner.m_sim.explosions;
    return state.valid() ? std::optional{std::move(state)} : std::nullopt;
}
std::optional<WorldDestructionCheckpoint::Prepared>
WorldDestructionCheckpoint::prepareRestore(const State& state) const {
    if (!state.valid() || !sameRoster(state) ||
        m_owner.m_checkpointGeneration == std::numeric_limits<u64>::max()) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->wrapper = this;
    data->owner = &m_owner;
    data->lifetime = m_lifetime;
    data->binding = m_owner.m_checkpointBinding;
    data->generation = m_owner.m_checkpointGeneration;
    data->state = state;
    return Prepared{std::move(data)};
}
bool WorldDestructionCheckpoint::canCommit(const Prepared& prepared) const {
    return prepared.m_data && prepared.m_data->wrapper == this &&
           prepared.m_data->owner == &m_owner && prepared.m_data->lifetime == m_lifetime &&
           prepared.m_data->binding == m_owner.m_checkpointBinding &&
           prepared.m_data->generation == m_owner.m_checkpointGeneration &&
           sameRoster(prepared.m_data->state);
}
bool WorldDestructionCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    m_owner.m_sim.destroyed.swap(data->state.destroyed);
    m_owner.m_sim.explosions.swap(data->state.explosions);
    ++m_owner.m_checkpointGeneration;
    return true;
}
bool WorldDestructionCheckpoint::restore(const State& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}
} // namespace gdl::game
