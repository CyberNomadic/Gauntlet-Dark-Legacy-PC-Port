#include "game/screens/OpponentSimulation.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace gdl::game {
namespace {

bool ordinaryKind(s32 kind) {
    return kind >= 0 && kind != kDeathKind && kind != kItKind && kind != kGarmBroodKind;
}

bool ordinary(const Enemies::Checkpoint& state) {
    return state.combatants.empty() && std::ranges::none_of(state.enemies, [](const auto& enemy) {
               return enemy.state != Enemies::State::Inactive &&
                      (!ordinaryKind(enemy.kind) || enemy.algorithm == kSuicideWay);
           });
}

bool supportedWay(s32 way) {
    switch (way) {
    case kSeekWay:
    case kSeekAliasWay:
    case kProwlWay:
    case kMirroredProwlWay:
    case kWanderWay:
    case kWanderOtherWay:
    case kChaseWay:
    case kLoiterWay:
    case kFleeWay:
    case kLurkWay:
    case kZigZagWay:
    case kSkirmishWay:
    case kBombWay:
    case kSkirmishBombWay:
    case kThrowWay:
    case kStandCastWay:
    case kRangeCastWay:
    case kCastWay:
    case kPatrolWay: return true;
    default: return false;
    }
}

bool unsupportedMinds(const OpponentSimulation::Checkpoint& state) {
    if (state.placements) {
        const auto upcoming = state.placements->activations();
        if (!upcoming || std::ranges::any_of(*upcoming, [](const auto& row) {
                return row.kind < 0 || row.kind >= kSwarmKindCount || row.kind == kGarmBroodKind ||
                       row.spawn.kind != row.kind || row.carried ||
                       !supportedWay(resolvedWayOf(row.spawn.kind, row.spawn.tier,
                                                   row.spawn.algorithm, row.spawn.patrolBirth));
            })) {
            return true;
        }
    }
    for (const auto& enemy : state.enemies.enemies) {
        if (enemy.state == Enemies::State::Inactive) {
            continue;
        }
        if (!supportedWay(enemy.algorithm)) {
            return true;
        }
    }
    std::vector<bool> resets(state.generators.generators.size(), false);
    for (const auto& event : state.enemies.generatorEvents) {
        if (event.kind == EnemyGeneratorEvent::Kind::PatrolHit ||
            event.kind == EnemyGeneratorEvent::Kind::PatrolDetached) {
            resets[static_cast<usize>(event.generator)] = true;
        }
    }
    for (usize index = 0; index < state.generators.generators.size(); ++index) {
        const auto& generator = state.generators.generators[index];
        if (generator.state <= 0 || generator.tier <= 0) {
            continue;
        }
        if (!ordinaryKind(generator.kind)) {
            return true;
        }
        const auto safeBirth = [&](s32 way) {
            return supportedWay(resolvedWayOf(generator.kind, generator.tier, way, false)) &&
                   supportedWay(resolvedWayOf(generator.kind, generator.tier, way, true));
        };
        // applyBroodEvents replaces a patrol's algorithm before spawning; the
        // queue can outlive the child or an earlier change to the generator.
        if (!safeBirth(resets[index] ? 0 : generator.algorithm)) {
            return true;
        }
    }
    return false;
}

bool boundedFlights(const EnemyMissiles& pool, f32 seconds) {
    constexpr f64 kNativeStep = 1.0 / 30;
    constexpr f64 kLeastSpatialStep = 0.05;
    constexpr f64 kPerFlightSteps = 256;
    constexpr f64 kTotalSteps = 4096;
    f64 total = 0;
    for (usize index = 0; index < pool.count(); ++index) {
        const auto& missile = pool.missile(index);
        const f64 duration =
            std::min(static_cast<f64>(seconds), static_cast<f64>(missile.secondsLeft));
        const f64 gravity = std::max(static_cast<f64>(missile.kind.weight), 0.0);
        const f64 speed =
            std::hypot(static_cast<f64>(missile.velocity.x), static_cast<f64>(missile.velocity.y),
                       static_cast<f64>(missile.velocity.z));
        // EnemyMissiles only accelerates by gravity (reflection negates velocity).
        // Bound speed across the entire frame, including its next spatial-step
        // lookahead. Twofold slack covers f32 rounding; held-to-flight transitions
        // and the final shortened substep each have an additional allowance.
        const f64 speedBound = speed + gravity * (duration + kNativeStep);
        const f64 stride = std::max(static_cast<f64>(missile.kind.radius) * 0.5, kLeastSpatialStep);
        const f64 steps = 2 * (std::ceil(duration / kNativeStep) +
                               std::ceil(duration * std::max(speedBound, 1.0) / stride) + 2);
        total += steps;
        if (!std::isfinite(steps) || steps > kPerFlightSteps || total > kTotalSteps) {
            return false;
        }
    }
    return true;
}

bool pendingInteraction(const OpponentSimulation::Checkpoint& state, bool combat = false) {
    const auto& enemies = state.enemies;
    const auto& missiles = state.missiles;
    // The complete combat caller owns players, rewards and impact presentation.
    // Cosmetic continuation must still refuse these gameplay/output queues.
    return (!combat &&
            (!enemies.blows.empty() || !enemies.losses.empty() || !enemies.feedback.empty())) ||
           !enemies.bursts.empty() || !enemies.deathEvents.empty() || !enemies.deathShots.empty() ||
           !enemies.tagged.empty() || !missiles.hits.empty() || !missiles.rockHits.empty() ||
           !missiles.pickupBlasts.empty();
}

bool ordinary(const EnemyView& player) {
    return !player.captured && !player.invisible && player.meleeWard == EnemyMeleeWard::None &&
           !player.antiDeath && !player.reflects && !player.it && !player.decoy &&
           player.heldDeath < 0;
}

bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool validFrame(const OpponentSimulation::Frame& frame) {
    if (frame.ticks < 1 || frame.ticks > 2 || !std::isfinite(frame.seconds) ||
        std::abs(frame.seconds * 60 - static_cast<f32>(frame.ticks)) > 0.0001f ||
        !std::isfinite(frame.missileSpeed) || frame.missileSpeed < 0 || frame.missileSpeed > 16 ||
        frame.players.size() > 4) {
        return false;
    }
    u32 seen = 0;
    for (const auto& player : frame.players) {
        if (player.player < 0 || player.player >= 4 || !finite(player.position) ||
            !std::isfinite(player.radius) || player.radius <= 0 || !std::isfinite(player.height) ||
            player.height <= 0 ||
            (player.collisionRadius &&
             (!std::isfinite(*player.collisionRadius) || *player.collisionRadius <= 0)) ||
            (player.collisionHeight &&
             (!std::isfinite(*player.collisionHeight) || *player.collisionHeight < 0))) {
            return false;
        }
        const u32 bit = 1U << static_cast<u32>(player.player);
        if ((seen & bit) != 0) {
            return false;
        }
        seen |= bit;
    }
    return true;
}

} // namespace

struct OpponentSimulation::Prepared::Data {
    Generators::Prepared generators;
    Enemies::Prepared enemies;
    EnemyMissiles::Prepared missiles;
    std::optional<PlacedEnemies::Prepared> placements;
};

OpponentSimulation::Prepared::Prepared(std::unique_ptr<Data> data) : m_data(std::move(data)) {}
OpponentSimulation::Prepared::Prepared(Prepared&&) noexcept = default;
OpponentSimulation::Prepared&
OpponentSimulation::Prepared::operator=(Prepared&&) noexcept = default;
OpponentSimulation::Prepared::~Prepared() = default;

std::optional<OpponentSimulation::Checkpoint>
OpponentSimulation::checkpoint(std::span<const EnemyMissiles::Resource> resources) const {
    auto generators = m_generators.checkpoint();
    auto enemies = m_enemies.checkpoint();
    auto missiles = m_missiles.checkpoint(resources);
    if (!generators || !enemies || !missiles) {
        return std::nullopt;
    }
    Checkpoint result{std::move(*generators), std::move(*enemies), std::move(*missiles)};
    if (m_placements != nullptr) {
        result.placements = m_placements->checkpoint();
        if (!result.placements) {
            return std::nullopt;
        }
    }
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}

std::optional<OpponentSimulation::Prepared>
OpponentSimulation::prepareRestore(const Checkpoint& state,
                                   std::span<const EnemyMissiles::Resource> resources) const {
    if (!state.valid() || state.placements.has_value() != (m_placements != nullptr)) {
        return std::nullopt;
    }
    auto generators = m_generators.prepareRestore(state.generators);
    auto enemies = m_enemies.prepareRestore(state.enemies);
    auto missiles = m_missiles.prepareRestore(state.missiles, resources);
    auto placements = m_placements != nullptr ? m_placements->prepareRestore(*state.placements)
                                              : std::optional<PlacedEnemies::Prepared>{};
    if (!generators || !enemies || !missiles || (m_placements != nullptr && !placements)) {
        return std::nullopt;
    }
    return Prepared{std::make_unique<Prepared::Data>(std::move(*generators), std::move(*enemies),
                                                     std::move(*missiles), std::move(placements))};
}

bool OpponentSimulation::canCommit(const Prepared& prepared) const {
    return prepared.m_data && m_generators.canCommit(prepared.m_data->generators) &&
           m_enemies.canCommit(prepared.m_data->enemies) &&
           m_missiles.canCommit(prepared.m_data->missiles) &&
           (prepared.m_data->placements.has_value() == (m_placements != nullptr)) &&
           (!prepared.m_data->placements || m_placements->canCommit(*prepared.m_data->placements));
}

bool OpponentSimulation::commitRestore(Prepared&& prepared) {
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
        (data->placements && !m_placements->commitRestore(std::move(*data->placements)))) {
        std::terminate();
    }
    return true;
}

bool OpponentSimulation::restore(const Checkpoint& state,
                                 std::span<const EnemyMissiles::Resource> resources) {
    auto prepared = prepareRestore(state, resources);
    return prepared && commitRestore(std::move(*prepared));
}

std::vector<MissileTarget> OpponentSimulation::targets(const Enemies& enemies,
                                                       std::span<const MissileTarget> critters,
                                                       std::span<const MissileTarget> bosses) {
    auto result = enemies.targets();
    for (auto target : critters) {
        target.id += kCritterIds;
        result.push_back(target);
    }
    for (auto target : bosses) {
        target.id += kBossIds;
        result.push_back(target);
    }
    return result;
}

std::vector<MissileStop> OpponentSimulation::advance(const Frame& frame,
                                                     const EnemyMissiles::SceneryBlast& scenery) {
    return advance(frame, scenery, nullptr, m_placements);
}

std::vector<MissileStop> OpponentSimulation::advance(const Frame& frame,
                                                     const EnemyMissiles::SceneryBlast& scenery,
                                                     EnemyMissileWork* work,
                                                     PlacedEnemies* placements) {
    if (placements != nullptr) {
        // This is the native watch phase's ordinary dispatcher. It intentionally
        // consumes each selected record even when the spawn fails or replaces a
        // visible pool slot; the shared owner preserves the original row order.
        placements->activate(
            [&](const auto& row) { m_enemies.spawn(row.spawn, {}, m_generators.obstacles()); });
    }
    m_generators.update(frame.ticks, m_enemies, frame.players, frame.fixtures, frame.timeStopped);
    auto boxes = m_generators.enemyObstacles();
    boxes.insert(boxes.end(), frame.fixtures.begin(), frame.fixtures.end());
    m_enemies.update(frame.ticks, frame.seconds, frame.players, boxes, &m_missiles,
                     frame.missileSpeed, frame.timeStopped, work);
    // A generator child or an existing caster can release during this very AI
    // update. Bound the resulting pool before any new flight is integrated.
    if (work != nullptr && (work->exhausted || !boundedFlights(m_missiles, frame.seconds))) {
        work->exhausted = true;
        return {};
    }
    std::vector<MissileStop> stops;
    for (const auto& box : m_generators.obstacles()) {
        stops.push_back(MissileStop::of(box));
    }
    if (frame.stops.empty()) {
        for (const auto& box : frame.fixtures) {
            stops.push_back(MissileStop::of(box));
        }
    } else {
        stops.insert(stops.end(), frame.stops.begin(), frame.stops.end());
    }
    m_missiles.update(frame.seconds, frame.collision, frame.players,
                      targets(m_enemies, frame.critters, frame.bosses), stops, scenery);
    return stops;
}

bool OpponentSimulation::supported(const Checkpoint& state, const Frame& frame) {
    // Cross-pool references must be checked before unsupportedMinds indexes the
    // generator roster with pending native brood events.
    return state.valid() && validFrame(frame) && frame.critters.empty() && frame.bosses.empty() &&
           std::ranges::all_of(frame.players,
                               [](const auto& player) { return ordinary(player); }) &&
           ordinary(state.enemies) && state.missiles.bursts.empty() && !pendingInteraction(state) &&
           !unsupportedMinds(state);
}

OpponentSimulation::Result
OpponentSimulation::speculate(const Frame& frame,
                              std::span<const EnemyMissiles::Resource> resources) {
    if (!validFrame(frame)) {
        return Result::Invalid;
    }
    const auto before = checkpoint(resources);
    if (!before) {
        return Result::Invalid;
    }
    if (!supported(*before, frame) || !boundedFlights(m_missiles, frame.seconds)) {
        return Result::Unsupported;
    }
    auto undo = prepareRestore(*before, resources);
    if (!undo) {
        return Result::Invalid;
    }
    EnemyMissileWork work;
    std::unique_ptr<PlacedEnemies> placements;
    if (before->placements) {
        placements = std::make_unique<PlacedEnemies>();
        placements->bind(before->placements->roster);
        if (!placements->restore(*before->placements)) {
            return Result::Invalid;
        }
    }
    // Do not invalidate the live owner's prepared undo by changing its watched
    // pending set. Commit the staged native activation only with the whole step.
    advance(frame, {}, &work, placements.get());
    if (work.exhausted) {
        if (!commitRestore(std::move(*undo))) {
            std::terminate();
        }
        return Result::Unsupported;
    }
    auto after = checkpoint(resources);
    if (after && placements) {
        after->placements = placements->checkpoint();
        if (!after->placements) {
            after.reset();
        }
    }
    if (after && after->valid() && ordinary(after->enemies) && after->missiles.bursts.empty() &&
        !pendingInteraction(*after)) {
        if (after->placements) {
            auto pending = m_placements->prepareRestore(*after->placements);
            if (!pending || !m_placements->commitRestore(std::move(*pending))) {
                if (!commitRestore(std::move(*undo))) {
                    std::terminate();
                }
                return Result::Invalid;
            }
        }
        return Result::Advanced;
    }
    if (!commitRestore(std::move(*undo))) {
        std::terminate(); // The private step never changes any bound resource.
    }
    return after ? Result::Interaction : Result::Invalid;
}

bool OpponentSimulation::combatSupported(const Checkpoint& state, const Frame& frame) {
    return state.valid() && validFrame(frame) && frame.critters.empty() && frame.bosses.empty() &&
           std::ranges::all_of(frame.players,
                               [](const auto& player) { return ordinary(player); }) &&
           ordinary(state.enemies) && state.missiles.bursts.empty() &&
           !pendingInteraction(state, true) && !unsupportedMinds(state);
}

OpponentSimulation::Result
OpponentSimulation::stageCombat(const Frame& frame,
                                std::span<const EnemyMissiles::Resource> resources) {
    if (!validFrame(frame)) {
        return Result::Invalid;
    }
    const auto before = checkpoint(resources);
    if (!before) {
        return Result::Invalid;
    }
    if (!combatSupported(*before, frame) || !boundedFlights(m_missiles, frame.seconds)) {
        return Result::Unsupported;
    }
    EnemyMissileWork work;
    advance(frame, {}, &work, m_placements);
    if (work.exhausted) {
        return Result::Unsupported;
    }
    const auto after = checkpoint(resources);
    if (!after) {
        return Result::Invalid;
    }
    return combatSupported(*after, frame) ? Result::Advanced : Result::Interaction;
}

} // namespace gdl::game
