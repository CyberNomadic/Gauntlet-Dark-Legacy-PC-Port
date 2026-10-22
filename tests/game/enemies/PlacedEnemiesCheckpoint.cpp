#include "game/enemies/PlacedEnemiesCheckpoint.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

namespace gdl::game {
namespace {
constexpr f32 kMagnitude = 1'000'000;
bool bounded(f32 value, f32 low = -kMagnitude, f32 high = kMagnitude) {
    return std::isfinite(value) && value >= low && value <= high;
}
bool bounded(const Vec3& value) {
    return bounded(value.x) && bounded(value.y) && bounded(value.z);
}
bool validView(const ViewVolume& view) {
    return bounded(view.position) && bounded(view.right) && bounded(view.up) &&
           bounded(view.forward) && bounded(view.sinX, -1, 1) && bounded(view.cosX, -1, 1) &&
           bounded(view.sinY, -1, 1) && bounded(view.cosY, -1, 1);
}
auto viewFields(const ViewVolume& view) {
    return std::tie(view.position, view.right, view.up, view.forward, view.sinX, view.cosX,
                    view.sinY, view.cosY);
}

std::optional<PlacedEnemies::Placement> activationOf(const PlacedEnemies::Placement& placement,
                                                     const std::optional<ViewVolume>& view,
                                                     const Vec3& attention) {
    const auto& position = placement.spawn.position;
    if (view && (!view->sees(position, placement.viewRadius) ||
                 glm::distance(attention, position) > PlacedEnemies::kWatchReach)) {
        return std::nullopt;
    }
    auto standing = placement;
    if (!view) {
        standing.spawn.priority = EnemySpawn::Priority::FreeSlotOnly;
    }
    return standing;
}
} // namespace

bool PlacedEnemiesCheckpoint::validPlacement(const PlacedEnemies::Placement& value) {
    const auto& [spawn, kind, facing, viewRadius, sight, carried] = value;
    return kind >= 0 && kind < kEnemyKindCount && spawn.kind >= 0 && spawn.kind < kEnemyKindCount &&
           spawn.tier >= 1 && spawn.tier <= 32767 && spawn.algorithm >= -32768 &&
           spawn.algorithm <= 32767 && spawn.zigZagSide >= -1 && spawn.zigZagSide <= 1 &&
           bounded(spawn.position) && bounded(spawn.direction) && bounded(spawn.clearance, 0) &&
           spawn.generator >= -1 && spawn.generator <= 1'000'000 &&
           bounded(spawn.throwInterval, 0, 3600) &&
           spawn.priority >= EnemySpawn::Priority::FreeSlotOnly &&
           spawn.priority <= EnemySpawn::Priority::Visible && bounded(spawn.sight) &&
           bounded(facing) && bounded(viewRadius, 0) && bounded(sight) &&
           (!carried || *carried <= 1'000'000);
}

bool PlacedEnemiesCheckpoint::State::valid() const {
    if (roster.size() > kMaxPlacements || pending.size() > roster.size() || !bounded(attention) ||
        (view && !validView(*view)) ||
        !std::ranges::all_of(roster, &PlacedEnemiesCheckpoint::validPlacement)) {
        return false;
    }
    for (usize index = 0; index < pending.size(); ++index) {
        if (pending[index] >= roster.size() ||
            (index > 0 && pending[index - 1] >= pending[index])) {
            return false;
        }
    }
    return true;
}

bool PlacedEnemiesCheckpoint::State::operator==(const State& other) const {
    return roster == other.roster && pending == other.pending && attention == other.attention &&
           view.has_value() == other.view.has_value() &&
           (!view || viewFields(*view) == viewFields(*other.view));
}

std::optional<std::vector<PlacedEnemies::Placement>>
PlacedEnemiesCheckpoint::State::activations() const {
    return activations(view, attention);
}

std::optional<std::vector<PlacedEnemies::Placement>>
PlacedEnemiesCheckpoint::State::activations(std::optional<ViewVolume> nextView,
                                            const Vec3& nextAttention) const {
    if (!valid() || !bounded(nextAttention) || (nextView && !validView(*nextView))) {
        return std::nullopt;
    }
    std::vector<PlacedEnemies::Placement> result;
    for (const usize index : pending) {
        if (auto standing = activationOf(roster[index], nextView, nextAttention)) {
            result.push_back(*standing);
        }
    }
    return result;
}

std::optional<PlacedEnemiesCheckpoint::State>
PlacedEnemiesCheckpoint::capture(const PlacedEnemies& owner) {
    if (!owner.m_bound || owner.m_roster.size() > State::kMaxPlacements) {
        return std::nullopt;
    }
    State result{owner.m_roster, owner.m_sim.pending, owner.m_sim.view, owner.m_sim.attention};
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}

struct PlacedEnemiesCheckpoint::Prepared::Data {
    PlacedEnemies::LocalSnapshot baseline;
    PlacedEnemies::LocalSnapshot target;
};

PlacedEnemiesCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
PlacedEnemiesCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
PlacedEnemiesCheckpoint::Prepared&
PlacedEnemiesCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
PlacedEnemiesCheckpoint::Prepared::~Prepared() = default;

std::optional<PlacedEnemiesCheckpoint::Prepared>
PlacedEnemiesCheckpoint::prepareRestore(const PlacedEnemies& owner, const State& state) {
    if (!owner.m_bound || !state.valid() || state.roster != owner.m_roster) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    owner.saveLocal(data->baseline);
    owner.saveLocal(data->target);
    data->target.m_values = {state.pending, state.view, state.attention};
    return Prepared{std::move(data)};
}

bool PlacedEnemiesCheckpoint::canCommit(const PlacedEnemies& owner, const Prepared& prepared) {
    if (!prepared.m_data || !owner.canRestoreLocal(prepared.m_data->target)) {
        return false;
    }
    PlacedEnemies::LocalSnapshot current;
    owner.saveLocal(current);
    return owner.equalLocal(prepared.m_data->baseline, current);
}

bool PlacedEnemiesCheckpoint::commitRestore(PlacedEnemies& owner, Prepared&& prepared) {
    if (!canCommit(owner, prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    owner.m_sim = std::move(data->target.m_values);
    ++owner.m_localEpoch; // Diagnostic state replacement invalidates native history.
    return true;
}

bool PlacedEnemiesCheckpoint::restore(PlacedEnemies& owner, const State& state) {
    auto prepared = prepareRestore(owner, state);
    return prepared && commitRestore(owner, std::move(*prepared));
}
} // namespace gdl::game
