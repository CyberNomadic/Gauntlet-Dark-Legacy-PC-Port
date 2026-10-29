#include "game/enemies/PlacedEnemies.h"

#include <algorithm>
#include <numeric>
#include <tuple>
#include <utility>

#include "engine/math/PortableMath.h"

namespace gdl::game {
namespace {
auto spawnFields(const EnemySpawn& spawn) {
    return std::tie(spawn.zigZagSide, spawn.kind, spawn.tier, spawn.algorithm, spawn.position,
                    spawn.direction, spawn.clearance, spawn.generator, spawn.placed,
                    spawn.patrolBirth, spawn.allBirthDirections, spawn.asleep, spawn.throwInterval,
                    spawn.priority, spawn.sight);
}

std::optional<PlacedEnemies::Placement> activationOf(const PlacedEnemies::Placement& placement,
                                                     const std::optional<ViewSet>& view) {
    const auto& position = placement.spawn.position;
    if (view && !view->sees(position, placement.viewRadius, PlacedEnemies::kWatchReach)) {
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
           std::tie(kind, facing, viewRadius, sight, carried, support, supportLocal) ==
               std::tie(other.kind, other.facing, other.viewRadius, other.sight, other.carried,
                        other.support, other.supportLocal);
}

void PlacedEnemies::saveLocal(LocalSnapshot& saved) const {
    saved.m_values = m_sim;
    saved.m_owner = m_lifetime;
    saved.m_epoch = m_localEpoch;
}
bool PlacedEnemies::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner.lock() == m_lifetime && saved.m_epoch == m_localEpoch;
}
bool PlacedEnemies::equalLocal(const LocalSnapshot& a, const LocalSnapshot& b) const {
    if (!canRestoreLocal(a) || !canRestoreLocal(b)) {
        return false;
    }
    const auto& x = a.m_values;
    const auto& y = b.m_values;
    return x.pending == y.pending && x.view == y.view;
}
bool PlacedEnemies::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_values;
    return true;
}

bool PlacedEnemies::appendStateDigest(NativeStateDigest& digest, const Simulation& state) const {
    digest.string("PlacedEnemies.1");
    digest.values(m_bound, m_roster, state);
    return digest.good();
}

bool PlacedEnemies::appendNativeDigest(NativeStateDigest& digest) const {
    return appendStateDigest(digest, m_sim);
}

bool PlacedEnemies::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    return appendStateDigest(digest, saved.m_values);
}

void PlacedEnemies::bind(std::vector<Placement> roster, const WorldCollision* collision) {
    std::vector<usize> pending(roster.size());
    std::iota(pending.begin(), pending.end(), usize{0});
    m_roster = std::move(roster);
    m_collision = collision;
    m_sim.pending = std::move(pending);
    m_sim.view.reset();
    m_bound = true;
    ++m_localEpoch;
}

void PlacedEnemies::clear() {
    m_roster.clear();
    m_collision = nullptr;
    m_sim.pending.clear();
    m_sim.view.reset();
    m_bound = false;
    ++m_localEpoch;
}

void PlacedEnemies::observe(std::optional<ViewVolume> view, const Vec3& attention) {
    m_sim.view = view ? std::optional{ViewSet::of(*view, attention)} : std::nullopt;
}

void PlacedEnemies::observeSet(const std::optional<ViewSet>& views) {
    m_sim.view = views;
}

bool PlacedEnemies::inView(const Vec3& position, f32 radius) const {
    return !m_sim.view || m_sim.view->sees(position, radius, kWatchReach);
}

std::optional<PlacedEnemies::Placement> PlacedEnemies::activation(usize index) const {
    Placement placement = m_roster[index];
    // Set on a moving floor, it is seen and born where that floor is now: the original
    // refreshes a parented item from its node every frame (fn_800606FC) before the sight
    // test and the birth (fn_80060114).
    if (placement.support >= 0 && m_collision != nullptr) {
        if (const auto transform = m_collision->objectTransform(placement.support)) {
            const Mat4 placed = *transform * placement.supportLocal;
            placement.spawn.position = Vec3{placed[3]};
            placement.facing = portable::atan2(placed[2].x, placed[2].z);
            placement.spawn.direction =
                Vec3{portable::sin(placement.facing), 0, portable::cos(placement.facing)};
        }
    }
    return activationOf(placement, m_sim.view);
}

std::vector<PlacedEnemies::Placement> PlacedEnemies::activations() const {
    std::vector<Placement> result;
    for (const usize index : m_sim.pending) {
        if (auto standing = activation(index)) {
            result.push_back(*standing);
        }
    }
    return result;
}

void PlacedEnemies::activate(const std::function<void(const Placement&)>& stand) {
    std::erase_if(m_sim.pending, [&](usize index) {
        const auto standing = activation(index);
        if (!standing) {
            return false;
        }
        stand(*standing);
        return true;
    });
}

} // namespace gdl::game
