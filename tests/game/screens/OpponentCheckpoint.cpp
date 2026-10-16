#include "game/screens/OpponentCheckpoint.h"

#include <exception>
#include <utility>

namespace gdl::game {
struct OpponentCheckpoint::Prepared::Data {
    Generators::Prepared generators;
    Enemies::Prepared enemies;
    EnemyMissiles::Prepared missiles;
    std::optional<PlacedEnemiesCheckpoint::Prepared> placements;
};

OpponentCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
OpponentCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
OpponentCheckpoint::Prepared&
OpponentCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
OpponentCheckpoint::Prepared::~Prepared() = default;

std::optional<OpponentCheckpoint::Checkpoint>
OpponentCheckpoint::checkpoint(std::span<const EnemyMissiles::Resource> resources) const {
    auto generators = m_generators.checkpoint();
    auto enemies = m_enemies.checkpoint();
    auto missiles = m_missiles.checkpoint(resources);
    if (!generators || !enemies || !missiles) {
        return std::nullopt;
    }
    Checkpoint result{std::move(*generators), std::move(*enemies), std::move(*missiles)};
    if (m_placements != nullptr) {
        result.placements = PlacedEnemiesCheckpoint::capture(*m_placements);
        if (!result.placements) {
            return std::nullopt;
        }
    }
    // Successful leaf captures are fully validated; retain every cross-pool
    // generator reference without repeating their internal roster scans.
    return result.validAggregate() ? std::optional{std::move(result)} : std::nullopt;
}

std::optional<OpponentCheckpoint::Prepared>
OpponentCheckpoint::prepareRestore(const Checkpoint& state,
                                   std::span<const EnemyMissiles::Resource> resources) const {
    if (!state.valid() || state.placements.has_value() != (m_placements != nullptr)) {
        return std::nullopt;
    }
    auto generators = m_generators.prepareRestore(state.generators);
    auto enemies = m_enemies.prepareRestore(state.enemies);
    auto missiles = m_missiles.prepareRestore(state.missiles, resources);
    auto placements = m_placements != nullptr
                          ? PlacedEnemiesCheckpoint::prepareRestore(*m_placements, *state.placements)
                          : std::optional<PlacedEnemiesCheckpoint::Prepared>{};
    if (!generators || !enemies || !missiles || (m_placements != nullptr && !placements)) {
        return std::nullopt;
    }
    return Prepared{std::make_unique<Prepared::Data>(std::move(*generators), std::move(*enemies),
                                                     std::move(*missiles), std::move(placements))};
}

bool OpponentCheckpoint::canCommit(const Prepared& prepared) const {
    return prepared.m_data && m_generators.canCommit(prepared.m_data->generators) &&
           m_enemies.canCommit(prepared.m_data->enemies) &&
           m_missiles.canCommit(prepared.m_data->missiles) &&
           (prepared.m_data->placements.has_value() == (m_placements != nullptr)) &&
           (!prepared.m_data->placements ||
            PlacedEnemiesCheckpoint::canCommit(*m_placements, *prepared.m_data->placements));
}

bool OpponentCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    // The prepared pools borrow immutable resources and no callbacks run here.
    // A refusal after all canCommit checks would violate the transaction
    // contract; returning a partly restored world would conceal that violation.
    if (!m_generators.commitRestore(std::move(data->generators)) ||
        !m_enemies.commitRestore(std::move(data->enemies)) ||
        !m_missiles.commitRestore(std::move(data->missiles)) ||
        (data->placements &&
         !PlacedEnemiesCheckpoint::commitRestore(*m_placements, std::move(*data->placements)))) {
        std::terminate();
    }
    return true;
}

bool OpponentCheckpoint::restore(const Checkpoint& state,
                                 std::span<const EnemyMissiles::Resource> resources) {
    auto prepared = prepareRestore(state, resources);
    return prepared && commitRestore(std::move(*prepared));
}

} // namespace gdl::game
