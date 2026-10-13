#include "game/screens/OpponentPrediction.h"

#include <algorithm>
#include <array>
#include <exception>
#include <functional>
#include <limits>
#include <utility>

#include "engine/core/Log.h"

#include "game/config/GameConfig.h"
#include "game/screens/OpponentPlacements.h"
#include "game/screens/OpponentResources.h"
#include "game/world/LevelWorld.h"

namespace gdl::game {

struct OpponentPrediction::Data {
    WorldCollision collision;
    Enemies enemies;
    Generators generators;
    EnemyMissiles missiles;
    PlacedEnemies placements;
    bool includePlacements = false;
    std::vector<EnemyMissiles::Resource> resources;
    EnemyScales scales;
    GeneratorScales breeding;
    s32 most = Enemies::kMost;
    LookoutRoute lookouts;
    Vec2 gridOrigin{0};
    f32 gridInverseWidth = 0;
    std::optional<OpponentState> authority;
    u64 epoch = 0;
    u64 baseline = 0;
    u64 tick = 0;
    bool active = false;

    OpponentSimulation simulation() {
        return {generators, enemies, missiles, includePlacements ? &placements : nullptr};
    }

    bool load(RenderDevice& device, const std::filesystem::path& root, LevelWorld& world,
              const Enemies& catalog, s32 initialPlayers, f32 gain) {
        const auto trusted = catalog.checkpoint();
        if (!world.built() || !trusted || initialPlayers < 1 || initialPlayers > 4 ||
            std::ranges::find(DifficultyConfig::kGains, gain) == DifficultyConfig::kGains.end()) {
            return false;
        }
        collision = world.collision();
        scales.players = initialPlayers;
        const auto* level = world.level();
        if (level != nullptr) {
            scales.health = level->tuning.enemyHealth;
            scales.speed = level->tuning.enemySpeedScale(gain);
            scales.sight = level->tuning.enemySightScale(gain);
            scales.damage = level->tuning.enemyDamage;
            scales.missileRate = level->tuning.enemyMissileRate;
            scales.missileAim = level->tuning.enemyMissileAim;
            scales.playerLevel = level->tuning.playerLevel;
            scales.bossEncounter = level->bossType >= 0;
            breeding.health = level->tuning.generatorHealth;
            breeding.rate = level->tuning.generatorRateScale(gain);
            breeding.most = level->tuning.generatorMostScale(gain);
            most = std::clamp(level->maxEnemies, 1, Enemies::kMost);
        }
        const auto seed = static_cast<u32>(std::hash<std::string>{}(world.ref().name));
        enemies.open(device, root, &collision, most, scales, seed);
        enemies.setContactGridBounds(world.layout().minBounds(), world.layout().maxBounds());
        enemies.setHazards(&world.hazards());
        lookouts = LookoutRoute::of(world.layout().locators());
        enemies.setLookouts(lookouts);
        // These kinds were resolved from local level assets by StageResources,
        // never from a packet or a live shooter instance. Simulation clocks and
        // borrowed projectile models belong to this independent stock owner.
        for (const auto& stock : trusted->stocks) {
            if (!enemies.loadKind(stock.kind)) {
                return false;
            }
        }
        const auto roster = level != nullptr ? std::span<const LevelEnemy>{level->enemies}
                                             : std::span<const LevelEnemy>{};
        const std::array lenders{&world.textures(), &world.items().textures,
                                 &world.realmItems().textures};
        if (!generators.bind(device, world.layout(), enemies, &collision, breeding, initialPlayers,
                             roster, world.ref().realmId, &world.items(), lenders)) {
            return false;
        }
        const auto local = enemies.checkpoint();
        if (!local || local->stocks.size() != trusted->stocks.size()) {
            return false;
        }
        for (usize index = 0; index < local->stocks.size(); ++index) {
            if (local->stocks[index].kind != trusted->stocks[index].kind) {
                return false;
            }
        }
        std::vector<PlacedEnemies::Placement> placed;
        const auto& infos = world.layout().itemInfos();
        for (const auto& instance : world.layout().itemInstances()) {
            if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size()) {
                continue;
            }
            auto row = OpponentPlacements::ordinary(
                instance, infos[static_cast<usize>(instance.info)], roster, initialPlayers,
                [&](s32 kind) {
                    // Only already-trusted ordinary stocks enter this first
                    // slice. A native pending Great-one/Death/IT row causes the
                    // complete immutable roster comparison to refuse replay.
                    return kind >= 0 && kind < kSwarmKindCount && kind != kGarmBroodKind &&
                           std::ranges::any_of(local->stocks, [kind](const auto& stock) {
                               return stock.kind == kind;
                           });
                });
            if (row) {
                placed.push_back(std::move(*row));
            }
        }
        placements.bind(std::move(placed));
        gridOrigin = local->contactGridOrigin;
        gridInverseWidth = local->contactGridInverseWidth;
        resources = OpponentResources::missiles(enemies);
        return simulation().checkpoint(resources).has_value();
    }

    bool trusted(const OpponentState& state) const {
        return state.valid() && state.enemies.scales == scales && state.enemies.most == most &&
               state.enemies.lookouts == lookouts &&
               state.enemies.contactGridOrigin == gridOrigin &&
               state.enemies.contactGridInverseWidth == gridInverseWidth &&
               state.generators.scales == breeding;
    }

    void resetReplay() {
        active = false;
        if (authority) {
            // No resource owner, bound collision address, or layout changes during
            // a pass. A failed rollback is an internal transaction violation.
            if (!simulation().restore(*authority, resources)) {
                std::terminate();
            }
            tick = baseline;
        }
    }
};

OpponentPrediction::OpponentPrediction() = default;
OpponentPrediction::~OpponentPrediction() = default;

bool OpponentPrediction::open(RenderDevice& device, const std::filesystem::path& root,
                              LevelWorld& world, const Enemies& catalog, s32 initialPlayers,
                              f32 difficultyGain) {
    try {
        auto next = std::make_unique<Data>();
        if (!next->load(device, root, world, catalog, initialPlayers, difficultyGain)) {
            return false;
        }
        m_data = std::move(next);
        return true;
    } catch (const std::exception& error) {
        log::warn("Opponent continuation resources: {}", error.what());
        return false;
    }
}

void OpponentPrediction::clear() {
    m_data.reset();
}

bool OpponentPrediction::loaded() const {
    return m_data != nullptr;
}

bool OpponentPrediction::begin(u64 epoch, u64 postStepTick, const OpponentState& authority,
                               const WorldCollision& collision) {
    if (!m_data) {
        return false;
    }
    auto& data = *m_data;
    data.active = false;
    if (epoch == 0 || epoch < data.epoch || (epoch == data.epoch && postStepTick < data.baseline) ||
        !data.trusted(authority)) {
        return false;
    }
    // Preparation resolves support IDs against the new collision. Restore the
    // old value on any refusal; the pool transaction itself changes nothing.
    auto nextCollision = collision;
    auto baseline = authority;
    auto oldCollision = std::move(data.collision);
    const bool previousPlacements = data.includePlacements;
    data.includePlacements = authority.placements.has_value();
    data.collision = std::move(nextCollision);
    try {
        auto simulation = data.simulation();
        auto prepared = simulation.prepareRestore(authority, data.resources);
        if (!prepared || !simulation.commitRestore(std::move(*prepared))) {
            data.collision = std::move(oldCollision);
            data.includePlacements = previousPlacements;
            return false;
        }
    } catch (...) {
        data.collision = std::move(oldCollision);
        data.includePlacements = previousPlacements;
        throw;
    }
    data.authority = std::move(baseline);
    data.epoch = epoch;
    data.baseline = postStepTick;
    data.tick = postStepTick;
    data.active = true;
    return true;
}

OpponentSimulation::Result
OpponentPrediction::advance(u64 tick, const OpponentSimulation::Frame& frame, bool held) {
    if (!m_data || !m_data->active) {
        return OpponentSimulation::Result::Invalid;
    }
    auto& data = *m_data;
    if (data.tick == std::numeric_limits<u64>::max() || tick != data.tick + 1 ||
        tick - data.baseline > kMaxTicks) {
        data.resetReplay();
        return OpponentSimulation::Result::Invalid;
    }
    auto native = frame;
    native.collision = &data.collision;
    const auto result = held ? OpponentSimulation::Result::Advanced
                             : data.simulation().speculate(native, data.resources);
    if (result != OpponentSimulation::Result::Advanced) {
        data.resetReplay();
        return result;
    }
    data.tick = tick;
    return result;
}

void OpponentPrediction::resetReplay() {
    if (m_data) {
        m_data->resetReplay();
    }
}

bool OpponentPrediction::active() const {
    return m_data != nullptr && m_data->active;
}
u64 OpponentPrediction::epoch() const {
    return m_data != nullptr ? m_data->epoch : 0;
}
u64 OpponentPrediction::tick() const {
    return m_data != nullptr ? m_data->tick : 0;
}
const Enemies* OpponentPrediction::enemies() const {
    return active() ? &m_data->enemies : nullptr;
}
const Generators* OpponentPrediction::generators() const {
    return active() ? &m_data->generators : nullptr;
}
const EnemyMissiles* OpponentPrediction::missiles() const {
    return active() ? &m_data->missiles : nullptr;
}
const WorldCollision* OpponentPrediction::collision() const {
    return active() ? &m_data->collision : nullptr;
}

std::vector<OpponentPrediction::KnownEnemy> OpponentPrediction::knownEnemies() const {
    std::vector<KnownEnemy> result;
    if (!active() || !m_data->authority) {
        return result;
    }
    const auto current = m_data->enemies.checkpoint();
    if (!current) {
        return result;
    }
    for (usize slot = 0; slot < current->enemies.size(); ++slot) {
        const auto& body = current->enemies[slot];
        const auto& original = m_data->authority->enemies.enemies[slot];
        if (body.state != Enemies::State::Inactive && original.state != Enemies::State::Inactive &&
            body.instance == original.instance) {
            result.push_back({static_cast<u32>(slot), body});
        }
    }
    return result;
}

std::optional<OpponentState> OpponentPrediction::checkpoint() const {
    return m_data != nullptr ? m_data->simulation().checkpoint(m_data->resources) : std::nullopt;
}

} // namespace gdl::game
