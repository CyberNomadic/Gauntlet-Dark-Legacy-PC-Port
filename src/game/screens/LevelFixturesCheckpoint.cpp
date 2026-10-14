#include "game/screens/LevelFixturesCheckpoint.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

#include "game/world/FixtureCheckpointDetail.h"

namespace gdl::game {
namespace {
constexpr usize kMaxHazards = 4096;
constexpr usize kMaxContacts = 65535;
using fixture_checkpoint::scalar;
using fixture_checkpoint::vector;
template <class T> bool unique(const std::vector<T>& values) {
    auto sorted = values;
    std::ranges::sort(sorted);
    return std::ranges::adjacent_find(sorted) == sorted.end();
}
template <class T>
bool timers(const std::vector<std::pair<T, f32>>& values, const std::vector<T>& contacts) {
    if (values.size() > contacts.size()) {
        return false;
    }
    auto sortedContacts = contacts;
    std::ranges::sort(sortedContacts);
    std::vector<T> ids;
    ids.reserve(values.size());
    for (const auto& [id, time] : values) {
        if (!scalar(time) || time < 0 || !std::ranges::binary_search(sortedContacts, id)) {
            return false;
        }
        ids.push_back(id);
    }
    std::ranges::sort(ids);
    return std::ranges::adjacent_find(ids) == ids.end();
}
} // namespace
bool LevelFixturesCheckpoint::State::valid() const {
    if (!chests.valid() || !gates.valid() || !traps.valid() || !barrels.valid() || !rocks.valid() ||
        !scenery.valid() || !rubble.valid() || clouds.size() > kMaxHazards ||
        blasts.size() > kMaxHazards || doomedChests.size() > chests.chests.size() ||
        !scalar(difficultyGain) || difficultyGain < 0 || !unique(doomedChests)) {
        return false;
    }
    for (const auto index : doomedChests) {
        if (index >= chests.chests.size()) {
            return false;
        }
    }
    for (const auto& cloud : clouds) {
        if (!vector(cloud.position) || !scalar(cloud.damage) || cloud.damage < 0 ||
            !scalar(cloud.secondsLeft) || cloud.secondsLeft < 0) {
            return false;
        }
    }
    usize contacts = 0;
    for (const auto& blast : blasts) {
        if (!vector(blast.position) || !scalar(blast.radius) || blast.radius < 0 ||
            !scalar(blast.damage) || blast.damage < 0 || !scalar(blast.seconds) ||
            blast.seconds <= 0 || !scalar(blast.elapsed) || blast.elapsed < 0 ||
            blast.players.size() > 4 || blast.reached.size() > kMaxContacts - contacts) {
            return false;
        }
        contacts += blast.reached.size();
        if (blast.opponents.size() > kMaxContacts - contacts) {
            return false;
        }
        contacts += blast.opponents.size();
        if (!unique(blast.players) || !unique(blast.reached) || !unique(blast.opponents) ||
            std::ranges::any_of(blast.players, [](usize value) { return value >= 4; }) ||
            !timers(blast.playerReady, blast.players) ||
            !timers(blast.opponentReady, blast.opponents)) {
            return false;
        }
    }
    return true;
}
struct LevelFixturesCheckpoint::Resources {
    LevelWorld* world;
    ItemArchive* weapons;
    EffectTrees* effects;
    LevelSoundscape* audio;
    RenderDevice* device;
    f32 difficultyGain;
    std::shared_ptr<const u8> binding;
    ChestsCheckpoint chests;
    LockedGatesCheckpoint gates;
    TrapsCheckpoint traps;
    BreakablesCheckpoint barrels;
    SafeRocksCheckpoint rocks;
    StaticSceneryCheckpoint scenery;
    RubbleCheckpoint rubble;
    Resources(LevelFixtures& owner, std::span<ItemArchive* const> archives)
        : world(&owner.m_resources->world), weapons(&owner.m_resources->weapons),
          effects(&owner.m_resources->effects), audio(&owner.m_resources->audio),
          device(&owner.m_resources->device), difficultyGain(owner.m_resources->difficultyGain),
          binding(owner.m_checkpointBinding), chests(owner.m_chests), gates(owner.m_gates),
          traps(owner.m_traps), barrels(owner.m_barrels), rocks(owner.m_safeRocks),
          scenery(owner.m_scenery), rubble(owner.m_rubble, archives) {}
};
struct LevelFixturesCheckpoint::Prepared::Data {
    const LevelFixturesCheckpoint* wrapper = nullptr;
    LevelFixtures* owner = nullptr;
    std::shared_ptr<const u8> lifetime, binding;
    u64 generation = 0;
    std::optional<ChestsCheckpoint::Prepared> chests;
    std::optional<LockedGatesCheckpoint::Prepared> gates;
    std::optional<TrapsCheckpoint::Prepared> traps;
    std::optional<BreakablesCheckpoint::Prepared> barrels;
    std::optional<SafeRocksCheckpoint::Prepared> rocks;
    std::optional<StaticSceneryCheckpoint::Prepared> scenery;
    std::optional<RubbleCheckpoint::Prepared> rubble;
    std::vector<LevelFixtures::GasCloud> clouds;
    std::vector<LevelFixtures::Blast> blasts;
    std::vector<usize> doomedChests;
};
LevelFixturesCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
LevelFixturesCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
LevelFixturesCheckpoint::Prepared&
LevelFixturesCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
LevelFixturesCheckpoint::Prepared::~Prepared() = default;
LevelFixturesCheckpoint::LevelFixturesCheckpoint(LevelFixtures& owner) : m_owner(owner) {}
LevelFixturesCheckpoint::~LevelFixturesCheckpoint() = default;
bool LevelFixturesCheckpoint::bound() const {
    if (!m_resources || !m_owner.m_resources) {
        return false;
    }
    const auto& a = *m_resources;
    const auto& b = *m_owner.m_resources;
    return a.binding == m_owner.m_checkpointBinding && a.world == &b.world &&
           a.weapons == &b.weapons && a.effects == &b.effects && a.audio == &b.audio &&
           a.device == &b.device && a.difficultyGain == b.difficultyGain;
}
bool LevelFixturesCheckpoint::bindResources() {
    if (!m_owner.m_resources) {
        return false;
    }
    auto& resources = *m_owner.m_resources;
    const std::array<ItemArchive*, 2> archives{&resources.world.items(),
                                               &resources.world.realmItems()};
    auto prepared = std::make_unique<Resources>(m_owner, archives);
    if (!prepared->traps.bindResources(resources.device) ||
        !prepared->rubble.bindResources(resources.device)) {
        return false;
    }
    m_resources = std::move(prepared);
    m_lifetime = std::make_shared<const u8>(0);
    return true;
}
bool LevelFixturesCheckpoint::binds(const LevelFixtures& owner) const {
    return &owner == &m_owner && bound();
}
bool LevelFixturesCheckpoint::binds(const LevelWorld& world, const ItemArchive& weapons,
                                    const EffectTrees& effects, const RenderDevice& device) const {
    return bound() && m_resources->world == &world && m_resources->weapons == &weapons &&
           m_resources->effects == &effects && m_resources->device == &device;
}
std::optional<LevelFixturesCheckpoint::State> LevelFixturesCheckpoint::checkpoint() const {
    if (!bound()) {
        return std::nullopt;
    }
    auto chests = m_resources->chests.checkpoint();
    auto gates = m_resources->gates.checkpoint();
    auto traps = m_resources->traps.checkpoint();
    auto barrels = m_resources->barrels.checkpoint();
    auto rocks = m_resources->rocks.checkpoint();
    auto scenery = m_resources->scenery.checkpoint();
    auto rubble = m_resources->rubble.checkpoint();
    if (!chests || !gates || !traps || !barrels || !rocks || !scenery || !rubble) {
        return std::nullopt;
    }
    State state;
    state.chests = std::move(*chests);
    state.gates = std::move(*gates);
    state.traps = std::move(*traps);
    state.barrels = std::move(*barrels);
    state.rocks = std::move(*rocks);
    state.scenery = std::move(*scenery);
    state.rubble = std::move(*rubble);
    state.doomedChests = m_owner.m_doomedChests;
    state.difficultyGain = m_resources->difficultyGain;
    for (const auto& cloud : m_owner.m_clouds) {
        state.clouds.push_back({cloud.position, cloud.damage, cloud.secondsLeft, cloud.effect});
    }
    for (const auto& blast : m_owner.m_blasts) {
        state.blasts.push_back({blast.position, blast.radius, blast.damage, blast.seconds,
                                blast.elapsed, blast.flags, blast.started, blast.done,
                                blast.players, blast.reached, blast.opponents, blast.playerReady,
                                blast.opponentReady});
    }
    if (!state.valid()) {
        return std::nullopt;
    }
    return state;
}
std::optional<LevelFixturesCheckpoint::Prepared>
LevelFixturesCheckpoint::prepareRestore(const State& state) const {
    if (!bound() || !state.valid() || state.difficultyGain != m_resources->difficultyGain ||
        m_owner.m_checkpointGeneration == std::numeric_limits<u64>::max()) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->wrapper = this;
    data->owner = &m_owner;
    data->lifetime = m_lifetime;
    data->binding = m_owner.m_checkpointBinding;
    data->generation = m_owner.m_checkpointGeneration;
    data->chests = m_resources->chests.prepareRestore(state.chests);
    data->gates = m_resources->gates.prepareRestore(state.gates);
    data->traps = m_resources->traps.prepareRestore(state.traps);
    data->barrels = m_resources->barrels.prepareRestore(state.barrels);
    data->rocks = m_resources->rocks.prepareRestore(state.rocks);
    data->scenery = m_resources->scenery.prepareRestore(state.scenery);
    data->rubble = m_resources->rubble.prepareRestore(state.rubble);
    if (!data->chests || !data->gates || !data->traps || !data->barrels || !data->rocks ||
        !data->scenery || !data->rubble) {
        return std::nullopt;
    }
    data->doomedChests = state.doomedChests;
    for (const auto& cloud : state.clouds) {
        data->clouds.push_back({cloud.position, cloud.damage, cloud.secondsLeft, cloud.effect});
    }
    for (const auto& blast : state.blasts) {
        data->blasts.push_back({blast.position, blast.radius, blast.damage, blast.seconds,
                                blast.elapsed, blast.flags, blast.started, blast.done,
                                blast.players, blast.reached, blast.opponents, blast.playerReady,
                                blast.opponentReady});
    }
    return Prepared{std::move(data)};
}
bool LevelFixturesCheckpoint::canCommit(const Prepared& prepared) const {
    if (!bound() || !prepared.m_data) {
        return false;
    }
    const auto& data = *prepared.m_data;
    return data.wrapper == this && data.owner == &m_owner && data.lifetime == m_lifetime &&
           data.binding == m_owner.m_checkpointBinding &&
           data.generation == m_owner.m_checkpointGeneration && data.chests &&
           m_resources->chests.canCommit(*data.chests) && data.gates &&
           m_resources->gates.canCommit(*data.gates) && data.traps &&
           m_resources->traps.canCommit(*data.traps) && data.barrels &&
           m_resources->barrels.canCommit(*data.barrels) && data.rocks &&
           m_resources->rocks.canCommit(*data.rocks) && data.scenery &&
           m_resources->scenery.canCommit(*data.scenery) && data.rubble &&
           m_resources->rubble.canCommit(*data.rubble);
}
bool LevelFixturesCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    // No callbacks, rebinds or allocations may interleave joint preflight and
    // commit. A child refusing here is an ownership-contract violation.
    if (!m_resources->chests.commitRestore(std::move(*data->chests)) ||
        !m_resources->gates.commitRestore(std::move(*data->gates)) ||
        !m_resources->traps.commitRestore(std::move(*data->traps)) ||
        !m_resources->barrels.commitRestore(std::move(*data->barrels)) ||
        !m_resources->rocks.commitRestore(std::move(*data->rocks)) ||
        !m_resources->scenery.commitRestore(std::move(*data->scenery)) ||
        !m_resources->rubble.commitRestore(std::move(*data->rubble))) {
        std::terminate();
    }
    m_owner.m_clouds.swap(data->clouds);
    m_owner.m_blasts.swap(data->blasts);
    m_owner.m_doomedChests.swap(data->doomedChests);
    ++m_owner.m_checkpointGeneration;
    return true;
}
bool LevelFixturesCheckpoint::restore(const State& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}
} // namespace gdl::game
