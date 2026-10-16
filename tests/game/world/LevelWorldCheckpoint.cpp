#include "game/world/LevelWorldCheckpoint.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <utility>

#include "engine/core/Log.h"

namespace gdl::game {
namespace {
using fixture_checkpoint::scalar;
using fixture_checkpoint::vector;
bool text(const std::string& value, usize limit) {
    return value.size() <= limit && value.find('\0') == std::string::npos;
}
} // namespace

bool LevelWorldCheckpoint::State::valid() const {
    return core.valid() && triggers.valid() && destruction.valid() && falling.valid() &&
           walls.valid() && pickups.valid() && rotators.valid() && validAggregate();
}

bool LevelWorldCheckpoint::State::validObservation() const {
    return core.valid() && triggers.valid() && destruction.valid() && falling.valid() &&
           walls.valid() && pickups.valid() && rotators.valid() && validAggregate(true);
}

bool LevelWorldCheckpoint::State::validAggregate(bool observation) const {
    if (!text(level.realm, 64) || !text(level.name, 64) || !text(level.title, 256) ||
        !text(level.directory, 512) || !text(level.items, 512) || !text(level.ownItems, 512) ||
        level.realmId < -1 || level.realmId > 255 || level.index < 0 || level.index > 65535 ||
        !textures.valid() || !(observation ? particles.validValues() : particles.valid()) ||
        particleSources.size() != particles.entries.size() || !lighting.valid() ||
        !litNow.valid() || playerCount < -1 || playerCount > 4 || !scalar(frameRemainder) ||
        frameRemainder < 0 || frameRemainder >= 1 || !scalar(textureAdvance) ||
        textureAdvance < 0 || textureAdvance > 450 || !scalar(ambientOffset) ||
        cues.size() > kMaxCues || destruction.objects.size() != core.placements.size() ||
        triggers.parents.size() != core.placements.size() ||
        (playerCount >= 0 &&
         (triggers.playerCount != playerCount || pickups.players != playerCount))) {
        return false;
    }
    for (const auto& cue : cues) {
        if (cue.kind > RotatorCue::Kind::Stopped || cue.rotator >= rotators.rotators.size() ||
            !vector(cue.position)) {
            return false;
        }
    }
    s32 previousSource = -1;
    for (const auto source : particleSources) {
        if (source <= previousSource || static_cast<usize>(source) >= core.placements.size()) {
            return false;
        }
        previousSource = source;
    }
    usize total = 0;
    for (const auto& entry : particles.entries) {
        if (entry.emitter.particles.size() > kMaxParticles - total) {
            return false;
        }
        total += entry.emitter.particles.size();
        if (entry.worldObject >= 0 &&
            static_cast<usize>(entry.worldObject) >= core.placements.size()) {
            return false;
        }
    }
    return true;
}

struct LevelWorldCheckpoint::Resources {
    std::shared_ptr<const u8> binding;
    WorldStateCheckpoint core;
    WorldDestructionCheckpoint destruction;
    FallingSceneryCheckpoint falling;
    DestructibleWallsCheckpoint walls;
    PlacedItemsCheckpoint pickups;
    RotatorsCheckpoint rotators;
    std::vector<const Texture*> particleTextures;
    std::vector<s32> particleSources;
    std::vector<usize> particleOrder;
    explicit Resources(LevelWorld& owner)
        : binding(owner.m_checkpointBinding),
          core(owner.m_scene, owner.m_collision, owner.m_worldAnimator, owner.m_checkpointBinding),
          destruction(owner.m_destruction), falling(owner.m_fallingScenery), walls(owner.m_walls),
          pickups(owner.m_placedItems), rotators(owner.m_rotators) {
        for (usize i = 0; i < owner.m_sim.particles.size(); ++i) {
            particleTextures.push_back(owner.m_sim.particles.textureOf(i));
            particleOrder.push_back(i);
        }
        std::ranges::sort(particleOrder, {},
                          [&](usize index) { return owner.m_sim.particles.sourceObject(index); });
        for (const auto index : particleOrder) {
            particleSources.push_back(owner.m_sim.particles.sourceObject(index));
        }
    }
};
struct LevelWorldCheckpoint::Prepared::Data {
    const LevelWorldCheckpoint* wrapper = nullptr;
    std::shared_ptr<const u8> lifetime;
    std::shared_ptr<Resources> resources;
    u64 generation = 0;
    std::optional<WorldStateCheckpoint::Prepared> core;
    std::optional<LevelTriggers::Prepared> triggers;
    std::optional<WorldDestructionCheckpoint::Prepared> destruction;
    std::optional<FallingSceneryCheckpoint::Prepared> falling;
    std::optional<DestructibleWallsCheckpoint::Prepared> walls;
    std::optional<PlacedItemsCheckpoint::Prepared> pickups;
    std::optional<RotatorsCheckpoint::Prepared> rotators;
    ParticleField particles;
    WorldLighting lighting, litNow;
    std::vector<RotatorCue> cues;
    TextureAnimator::Checkpoint textures;
    s32 playerCount = -1;
    f32 frameRemainder = 0, textureAdvance = 0, ambientOffset = 0;
    bool particlesAdvanced = false;
};
LevelWorldCheckpoint::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
LevelWorldCheckpoint::Prepared::Prepared(Prepared&&) noexcept = default;
LevelWorldCheckpoint::Prepared&
LevelWorldCheckpoint::Prepared::operator=(Prepared&&) noexcept = default;
LevelWorldCheckpoint::Prepared::~Prepared() = default;
LevelWorldCheckpoint::LevelWorldCheckpoint(LevelWorld& owner) : m_owner(owner) {}
LevelWorldCheckpoint::~LevelWorldCheckpoint() = default;

void LevelWorldCheckpoint::clearResources() {
    m_resources.reset();
}
bool LevelWorldCheckpoint::bound(bool observation) const {
    if (!m_resources || m_resources->binding != m_owner.m_checkpointBinding || !m_owner.built() ||
        (!observation &&
         (m_owner.m_skorneArena.size() != 0 || m_owner.m_skorneArena.phase() != 0)) ||
        m_resources->particleTextures.size() != m_owner.m_sim.particles.size()) {
        return false;
    }
    for (usize i = 0; i < m_resources->particleTextures.size(); ++i) {
        if (m_resources->particleTextures[i] != m_owner.m_sim.particles.textureOf(i) ||
            m_resources->particleSources[i] !=
                m_owner.m_sim.particles.sourceObject(m_resources->particleOrder[i])) {
            return false;
        }
    }
    return true;
}
bool LevelWorldCheckpoint::bindResources(RenderDevice& device) {
    return bindResources(device, false);
}
bool LevelWorldCheckpoint::bindObservation(RenderDevice& device) {
    return bindResources(device, true);
}
bool LevelWorldCheckpoint::bindResources(RenderDevice& device, bool observation) {
    clearResources();
    if (!m_owner.built() || (!observation && (m_owner.m_skorneArena.size() != 0 ||
                                              m_owner.m_skorneArena.phase() != 0))) {
        log::warn("World observation {}: outside ordinary checkpoint domain (built {}, arena {} "
                  "phase {})",
                  m_owner.ref().name, m_owner.built(), m_owner.m_skorneArena.size(),
                  m_owner.m_skorneArena.phase());
        return false;
    }
    auto resources = std::make_shared<Resources>(m_owner);
    if (!resources->core.bind(m_owner.m_textureAnimator, m_owner.m_layout)) {
        log::warn("World observation {}: core binding refused", m_owner.ref().name);
        return false;
    }
    if (!resources->pickups.bindResources(device)) {
        log::warn("World observation {}: pickup binding refused", m_owner.ref().name);
        return false;
    }
    m_resources = std::move(resources);
    if (!capture(nullptr, nullptr, observation)) {
        clearResources();
        return false;
    }
    return true;
}
bool LevelWorldCheckpoint::binds(const LevelWorld& owner) const {
    return &owner == &m_owner && bound();
}
bool LevelWorldCheckpoint::bindsObservation(const LevelWorld& owner) const {
    return &owner == &m_owner && bound(true);
}
std::optional<LevelWorldCheckpoint::State> LevelWorldCheckpoint::observe() const {
    return capture(nullptr, nullptr, true);
}
std::optional<usize> LevelWorldCheckpoint::placeItemRecord(s32 record, const Vec3& position,
                                                           s32 amount) {
    if (!bound()) {
        return std::nullopt;
    }
    return m_resources->pickups.placeRecord(
        record, position, m_owner.m_collision.loaded() ? &m_owner.m_collision : nullptr, amount);
}
bool LevelWorldCheckpoint::hasCollisionObject(s32 object) const {
    return bound() && m_resources->core.hasCollisionObject(object);
}
std::optional<std::vector<Pickup>>
LevelWorldCheckpoint::collectItems(std::span<const Collector> collectors,
                                   const StagedPickupJudge& judge) {
    return bound() ? m_resources->pickups.collect(collectors, judge) : std::nullopt;
}
std::span<const s32> LevelWorldCheckpoint::collisionObjects() const {
    return bound() ? m_resources->core.collisionObjects() : std::span<const s32>{};
}
std::optional<LevelWorldCheckpoint::State>
LevelWorldCheckpoint::checkpoint(CaptureTimings* timings) const {
    return capture(nullptr, timings);
}
std::optional<LevelWorldCheckpoint::State>
LevelWorldCheckpoint::checkpoint(State&& reusable, CaptureTimings* timings) const {
    return capture(&reusable, timings);
}
std::optional<LevelWorldCheckpoint::State>
LevelWorldCheckpoint::capture(State* reusable, CaptureTimings* timings, bool observation) const {
    if (timings) {
        *timings = {};
    }
    using Clock = std::chrono::steady_clock;
    const auto elapsed = [](Clock::time_point begin) {
        return static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
    };
    const auto measured = [&](CaptureOwner owner, auto&& operation) {
        if (!timings) {
            return operation();
        }
        const auto begin = Clock::now();
        auto result = operation();
        (*timings)[static_cast<usize>(owner)] = elapsed(begin);
        return result;
    };
    if (!bound(observation) || (reusable && reusable->level != m_owner.m_ref)) {
        return std::nullopt;
    }
    auto core = measured(CaptureOwner::Core, [&] { return m_resources->core.checkpoint(); });
    if (reusable && core &&
        (reusable->core.layout != core->layout || reusable->core.catalog != core->catalog ||
         reusable->core.resourceCount != core->resourceCount ||
         reusable->particleSources != m_resources->particleSources)) {
        return std::nullopt;
    }
    auto triggers =
        measured(CaptureOwner::Triggers, [&] { return m_owner.m_triggers.checkpoint(); });
    auto destruction =
        measured(CaptureOwner::Destruction, [&] { return m_resources->destruction.checkpoint(); });
    auto falling =
        measured(CaptureOwner::Falling, [&] { return m_resources->falling.checkpoint(); });
    auto walls = measured(CaptureOwner::Walls, [&] { return m_resources->walls.checkpoint(); });
    auto pickups = measured(CaptureOwner::Pickups, [&] {
        return reusable ? m_resources->pickups.checkpoint(std::move(reusable->pickups))
                        : m_resources->pickups.checkpoint();
    });
    auto rotators =
        measured(CaptureOwner::Rotators, [&] { return m_resources->rotators.checkpoint(); });
    if (!core || !triggers || !destruction || !falling || !walls || !pickups || !rotators) {
        log::warn("World observation {}: child capture refused: core={} triggers={} destruction={} "
                  "falling={} walls={} pickups={} rotators={}",
                  m_owner.ref().name, core.has_value(), triggers.has_value(),
                  destruction.has_value(), falling.has_value(), walls.has_value(),
                  pickups.has_value(), rotators.has_value());
        return std::nullopt;
    }
    State state;
    state.level = m_owner.m_ref;
    state.core = std::move(*core);
    state.triggers = std::move(*triggers);
    state.destruction = std::move(*destruction);
    state.falling = std::move(*falling);
    state.walls = std::move(*walls);
    state.pickups = std::move(*pickups);
    state.rotators = std::move(*rotators);
    const auto ambientBegin = timings ? Clock::now() : Clock::time_point{};
    state.textures = m_owner.m_textureAnimator.checkpoint();
    state.particles = m_owner.m_sim.particles.checkpoint();
    auto nativeEntries = std::move(state.particles.entries);
    for (const auto index : m_resources->particleOrder) {
        state.particles.entries.push_back(std::move(nativeEntries[index]));
    }
    state.particleSources = m_resources->particleSources;
    state.lighting = WorldStateCheckpoint::Light::capture(m_owner.m_sim.lighting);
    state.litNow = WorldStateCheckpoint::Light::capture(m_owner.m_sim.litNow);
    for (const auto& cue : m_owner.m_sim.rotatorCues) {
        state.cues.push_back({cue.kind, cue.rotator, cue.position});
    }
    state.playerCount = m_owner.m_sim.playerCount;
    state.frameRemainder = m_owner.m_sim.frameRemainder;
    state.textureAdvance = m_owner.m_sim.textureAdvance;
    state.ambientOffset = m_owner.m_sim.ambientOffset;
    state.particlesAdvanced = m_owner.m_sim.particlesAdvanced;
    if (timings) {
        (*timings)[static_cast<usize>(CaptureOwner::Ambient)] = elapsed(ambientBegin);
    }
    // Each optional child capture above already validated its complete value.
    // Raw clocks/particles and every cross-child relation still belong here.
    if (!measured(CaptureOwner::Aggregate, [&] { return state.validAggregate(observation); })) {
        log::warn("World observation {}: aggregate refused: players={}/{}/{} objects={}/{}/{} "
                  "particles={}/{} valid={} textures={} lighting={}/{} remainder={} "
                  "textureAdvance={} ambient={}",
                  m_owner.ref().name, state.playerCount, state.triggers.playerCount,
                  state.pickups.players, state.core.placements.size(),
                  state.destruction.objects.size(), state.triggers.parents.size(),
                  state.particleSources.size(), state.particles.entries.size(),
                  state.particles.valid(), state.textures.valid(), state.lighting.valid(),
                  state.litNow.valid(), state.frameRemainder, state.textureAdvance,
                  state.ambientOffset);
        s32 previous = -1;
        for (const auto source : state.particleSources) {
            if (source <= previous || static_cast<usize>(source) >= state.core.placements.size()) {
                log::warn("World observation {}: particle source {} after {} outside unique "
                          "placement roster",
                          m_owner.ref().name, source, previous);
                break;
            }
            previous = source;
        }
        return std::nullopt;
    }
    return state;
}
std::optional<LevelWorldCheckpoint::Prepared>
LevelWorldCheckpoint::prepareRestore(const State& state) const {
    if (!bound() || !state.valid() || state.level != m_owner.m_ref ||
        m_owner.m_checkpointGeneration == std::numeric_limits<u64>::max() ||
        state.particleSources != m_resources->particleSources) {
        return std::nullopt;
    }
    auto nativeParticles = state.particles;
    for (usize i = 0; i < m_resources->particleOrder.size(); ++i) {
        nativeParticles.entries[m_resources->particleOrder[i]] = state.particles.entries[i];
    }
    if (!m_owner.m_sim.particles.canRestore(nativeParticles)) {
        return std::nullopt;
    }
    auto data = std::make_unique<Prepared::Data>();
    data->wrapper = this;
    data->lifetime = m_lifetime;
    data->resources = m_resources;
    data->generation = m_owner.m_checkpointGeneration;
    data->core = m_resources->core.prepareRestore(state.core);
    data->triggers = m_owner.m_triggers.prepareRestore(state.triggers);
    data->destruction = m_resources->destruction.prepareRestore(state.destruction);
    data->falling = m_resources->falling.prepareRestore(state.falling);
    data->walls = m_resources->walls.prepareRestore(state.walls);
    data->pickups = m_resources->pickups.prepareRestore(state.pickups);
    data->rotators = m_resources->rotators.prepareRestore(state.rotators);
    if (!data->core || !data->triggers || !data->destruction || !data->falling || !data->walls ||
        !data->pickups || !data->rotators) {
        return std::nullopt;
    }
    data->particles = m_owner.m_sim.particles;
    if (!data->particles.restore(nativeParticles)) {
        return std::nullopt;
    }
    data->lighting = state.lighting.lighting();
    data->litNow = state.litNow.lighting();
    for (const auto& cue : state.cues) {
        data->cues.push_back({cue.kind, cue.rotator, cue.position});
    }
    data->textures = state.textures;
    data->playerCount = state.playerCount;
    data->frameRemainder = state.frameRemainder;
    data->textureAdvance = state.textureAdvance;
    data->ambientOffset = state.ambientOffset;
    data->particlesAdvanced = state.particlesAdvanced;
    Prepared prepared{std::move(data)};
    return canCommit(prepared) ? std::optional{std::move(prepared)} : std::nullopt;
}
bool LevelWorldCheckpoint::canCommit(const Prepared& prepared) const {
    if (!bound() || !prepared.m_data) {
        return false;
    }
    const auto& data = *prepared.m_data;
    return data.wrapper == this && data.lifetime == m_lifetime && data.resources == m_resources &&
           data.generation == m_owner.m_checkpointGeneration &&
           data.generation != std::numeric_limits<u64>::max() && data.core &&
           m_resources->core.canCommit(*data.core) && data.triggers &&
           m_owner.m_triggers.canCommit(*data.triggers) && data.destruction &&
           m_resources->destruction.canCommit(*data.destruction) && data.falling &&
           m_resources->falling.canCommit(*data.falling) && data.walls &&
           m_resources->walls.canCommit(*data.walls) && data.pickups &&
           m_resources->pickups.canCommit(*data.pickups) && data.rotators &&
           m_resources->rotators.canCommit(*data.rotators);
}
bool LevelWorldCheckpoint::commitRestore(Prepared&& prepared) {
    if (!canCommit(prepared)) {
        return false;
    }
    auto data = std::move(prepared.m_data);
    // Native updates may intervene before this preflight (an undo), but no callback,
    // rebind or foreign restore may interleave these disjoint prepared commits.
    if (!m_resources->core.commitRestore(std::move(*data->core)) ||
        !m_owner.m_triggers.commitRestore(std::move(*data->triggers)) ||
        !m_resources->destruction.commitRestore(std::move(*data->destruction)) ||
        !m_resources->falling.commitRestore(std::move(*data->falling)) ||
        !m_resources->walls.commitRestore(std::move(*data->walls)) ||
        !m_resources->pickups.commitRestore(std::move(*data->pickups)) ||
        !m_resources->rotators.commitRestore(std::move(*data->rotators)) ||
        !m_owner.m_textureAnimator.restore(data->textures)) {
        std::terminate();
    }
    std::swap(m_owner.m_sim.particles, data->particles);
    std::swap(m_owner.m_sim.lighting, data->lighting);
    std::swap(m_owner.m_sim.litNow, data->litNow);
    m_owner.m_sim.rotatorCues.swap(data->cues);
    m_owner.m_sim.playerCount = data->playerCount;
    m_owner.m_sim.frameRemainder = data->frameRemainder;
    m_owner.m_sim.textureAdvance = data->textureAdvance;
    m_owner.m_sim.ambientOffset = data->ambientOffset;
    m_owner.m_sim.particlesAdvanced = data->particlesAdvanced;
    ++m_owner.m_checkpointGeneration;
    return true;
}
bool LevelWorldCheckpoint::restore(const State& state) {
    auto prepared = prepareRestore(state);
    return prepared && commitRestore(std::move(*prepared));
}
} // namespace gdl::game
