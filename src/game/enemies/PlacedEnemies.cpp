#include "game/enemies/PlacedEnemies.h"

#include <algorithm>
#include <cmath>
#include <numeric>
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
auto spawnFields(const EnemySpawn& spawn) {
    return std::tie(spawn.zigZagSide, spawn.kind, spawn.tier, spawn.algorithm, spawn.position,
                    spawn.direction, spawn.clearance, spawn.generator, spawn.placed,
                    spawn.patrolBirth, spawn.allBirthDirections, spawn.asleep, spawn.throwInterval,
                    spawn.priority, spawn.sight);
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

bool PlacedEnemies::Placement::operator==(const Placement& other) const {
    return spawnFields(spawn) == spawnFields(other.spawn) &&
           std::tie(kind, facing, viewRadius, sight, carried) ==
               std::tie(other.kind, other.facing, other.viewRadius, other.sight, other.carried);
}

bool PlacedEnemies::Placement::valid() const {
    return kind >= 0 && kind < kEnemyKindCount && spawn.kind >= 0 && spawn.kind < kEnemyKindCount &&
           spawn.tier >= 1 && spawn.tier <= 32767 && spawn.algorithm >= -32768 &&
           spawn.algorithm <= 32767 && spawn.zigZagSide >= -1 && spawn.zigZagSide <= 1 &&
           bounded(spawn.position) && bounded(spawn.direction) && bounded(spawn.clearance, 0) &&
           spawn.generator >= -1 && spawn.generator <= 1'000'000 &&
           bounded(spawn.throwInterval, 0, EnemyAnimator::Checkpoint::kMaxThrowInterval) &&
           spawn.priority >= EnemySpawn::Priority::FreeSlotOnly &&
           spawn.priority <= EnemySpawn::Priority::Visible && bounded(spawn.sight) &&
           bounded(facing) && bounded(viewRadius, 0) && bounded(sight) &&
           (!carried || *carried <= 1'000'000);
}

bool PlacedEnemies::Checkpoint::valid() const {
    if (roster.size() > kMaxPlacements || pending.size() > roster.size() || !bounded(attention) ||
        (view && !validView(*view)) || !std::ranges::all_of(roster, &Placement::valid)) {
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

bool PlacedEnemies::Checkpoint::operator==(const Checkpoint& other) const {
    return roster == other.roster && pending == other.pending && attention == other.attention &&
           view.has_value() == other.view.has_value() &&
           (!view || viewFields(*view) == viewFields(*other.view));
}

std::optional<std::vector<PlacedEnemies::Placement>>
PlacedEnemies::Checkpoint::activations() const {
    return activations(view, attention);
}

std::optional<std::vector<PlacedEnemies::Placement>>
PlacedEnemies::Checkpoint::activations(std::optional<ViewVolume> nextView,
                                       const Vec3& nextAttention) const {
    if (!valid() || !bounded(nextAttention) || (nextView && !validView(*nextView))) {
        return std::nullopt;
    }
    std::vector<Placement> result;
    for (const usize index : pending) {
        if (auto standing = activationOf(roster[index], nextView, nextAttention)) {
            result.push_back(std::move(*standing));
        }
    }
    return result;
}

void PlacedEnemies::bind(std::vector<Placement> roster) {
    std::vector<usize> pending(roster.size());
    std::iota(pending.begin(), pending.end(), usize{0});
    m_roster = std::move(roster);
    m_pending = std::move(pending);
    m_view.reset();
    m_bound = true;
    ++m_generation;
    ++m_rewindGeneration;
}

void PlacedEnemies::clear() {
    m_roster.clear();
    m_pending.clear();
    m_view.reset();
    m_bound = false;
    ++m_generation;
    ++m_rewindGeneration;
}

void PlacedEnemies::observe(std::optional<ViewVolume> view, const Vec3& attention) {
    m_view = std::move(view);
    m_attention = attention;
    ++m_generation;
}

bool PlacedEnemies::inView(const Vec3& position, f32 radius) const {
    return !m_view ||
           (m_view->sees(position, radius) && glm::distance(m_attention, position) <= kWatchReach);
}

std::optional<PlacedEnemies::Placement> PlacedEnemies::activation(usize index) const {
    return activationOf(m_roster[index], m_view, m_attention);
}

std::vector<PlacedEnemies::Placement> PlacedEnemies::activations() const {
    std::vector<Placement> result;
    for (const usize index : m_pending) {
        if (auto standing = activation(index)) {
            result.push_back(std::move(*standing));
        }
    }
    return result;
}

void PlacedEnemies::activate(const std::function<void(const Placement&)>& stand) {
    ++m_generation;
    std::erase_if(m_pending, [&](usize index) {
        const auto standing = activation(index);
        if (!standing) {
            return false;
        }
        stand(*standing);
        return true;
    });
}

std::optional<PlacedEnemies::Checkpoint> PlacedEnemies::checkpoint() const {
    if (!m_bound || m_roster.size() > Checkpoint::kMaxPlacements) {
        return std::nullopt;
    }
    Checkpoint result{m_roster, m_pending, m_view, m_attention};
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}

struct PlacedEnemies::Prepared::Data {
    const PlacedEnemies* owner = nullptr;
    std::shared_ptr<const u8> lifetime;
    u64 generation = 0;
    u64 rewindGeneration = 0;
    std::vector<usize> pending;
    std::optional<ViewVolume> view;
    Vec3 attention{0};
};

PlacedEnemies::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
PlacedEnemies::Prepared::Prepared(Prepared&&) noexcept = default;
PlacedEnemies::Prepared& PlacedEnemies::Prepared::operator=(Prepared&&) noexcept = default;
PlacedEnemies::Prepared::~Prepared() = default;
PlacedEnemies::Rewind::Rewind(Prepared&& state) : m_state(std::move(state)) {}
PlacedEnemies::Rewind::Rewind(Rewind&&) noexcept = default;
PlacedEnemies::Rewind& PlacedEnemies::Rewind::operator=(Rewind&&) noexcept = default;
PlacedEnemies::Rewind::~Rewind() = default;

std::optional<PlacedEnemies::Prepared>
PlacedEnemies::prepareRestore(const Checkpoint& state) const {
    if (!m_bound || !state.valid() || state.roster != m_roster) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->owner = this;
    data->lifetime = m_lifetime;
    data->generation = m_generation;
    data->rewindGeneration = m_rewindGeneration;
    data->pending = state.pending;
    data->view = state.view;
    data->attention = state.attention;
    return Prepared{std::move(data)};
}

bool PlacedEnemies::canCommit(const Prepared& prepared) const {
    return m_bound && prepared.m_data && prepared.m_data->owner == this &&
           prepared.m_data->lifetime == m_lifetime && prepared.m_data->generation == m_generation;
}

bool PlacedEnemies::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    commitReady(std::move(prepared));
    return true;
}
void PlacedEnemies::commitReady(Prepared&& prepared) {
    auto data = std::move(prepared.m_data);
    m_pending.swap(data->pending);
    m_view = data->view;
    m_attention = data->attention;
    ++m_generation;
    ++m_rewindGeneration;
}

bool PlacedEnemies::restore(const Checkpoint& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}

std::optional<PlacedEnemies::Rewind> PlacedEnemies::prepareRewind() const {
    const auto state = checkpoint();
    return state ? prepareRewind(*state) : std::nullopt;
}
std::optional<PlacedEnemies::Rewind> PlacedEnemies::prepareRewind(const Checkpoint& target) const {
    auto prepared = prepareRestore(target);
    return prepared ? std::optional{Rewind{std::move(*prepared)}} : std::nullopt;
}
bool PlacedEnemies::canRewind(const Rewind& rewind) const {
    const auto& data = rewind.m_state.m_data;
    return m_bound && data && data->owner == this && data->lifetime == m_lifetime &&
           data->rewindGeneration == m_rewindGeneration;
}
bool PlacedEnemies::rewind(Rewind&& rewind) {
    if (!canRewind(rewind)) {
        return false;
    }
    commitReady(std::move(rewind.m_state));
    return true;
}

} // namespace gdl::game
