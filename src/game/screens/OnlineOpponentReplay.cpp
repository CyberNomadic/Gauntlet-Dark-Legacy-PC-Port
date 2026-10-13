#include "game/screens/OnlineOpponentReplay.h"

#include <algorithm>
#include <cmath>

namespace gdl::game {
namespace {
bool ordinaryAction(PlayerAnimator::Action action) {
    using Action = PlayerAnimator::Action;
    return action <= Action::Run2 ||
           (action >= Action::DefendRaise && action <= Action::DefendLower) ||
           (action >= Action::StrafeForward1 && action <= Action::StrafeRight2);
}

std::vector<EnemyCombatState> picture(const OpponentPrediction& prediction,
                                      std::span<const u64> instances) {
    std::vector<EnemyCombatState> result;
    const auto* enemies = prediction.enemies();
    if (enemies == nullptr) {
        return result;
    }
    for (usize index = 0; index < instances.size(); ++index) {
        if (instances[index] == 0) {
            continue;
        }
        const auto slot = static_cast<s32>(index);
        const auto observation = enemies->observe(slot);
        const auto* animator = enemies->animatorOf(slot);
        if (!observation || animator == nullptr || observation->instance != instances[index]) {
            continue;
        }
        EnemyCombatState shown;
        shown.instance = observation->instance;
        shown.kind = static_cast<u32>(enemies->kindOf(slot));
        shown.tier = static_cast<u32>(enemies->tierOf(slot));
        shown.variant = static_cast<u32>(enemies->variantOf(slot));
        shown.life = enemies->dying(slot)  ? ReplicaEnemyLife::Dying
                     : observation->asleep ? ReplicaEnemyLife::Asleep
                                           : ReplicaEnemyLife::Active;
        shown.position = enemies->positionOf(slot);
        shown.yaw = enemies->yawOf(slot);
        shown.animation = {static_cast<u32>(animator->action()), animator->player().sequence(),
                           animator->player().generation(), animator->player().frame(),
                           animator->player().transition()};
        shown.health = observation->health;
        shown.fullHealth = observation->fullHealth;
        shown.alpha = observation->alpha;
        shown.scale = observation->scale;
        shown.hitFlash = observation->hitFlash;
        shown.deathSkin = observation->deathSkin;
        shown.deathFrame = observation->deathFrame;
        result.push_back(shown);
    }
    std::ranges::sort(result, {}, &EnemyCombatState::instance);
    return result;
}
} // namespace

void OnlineOpponentReplay::begin(const MatchContext& context, u8 localPeer,
                                 OpponentPrediction& prediction, const WorldScene& scene,
                                 const WorldLayout& layout, const WorldCollision& collision) {
    clear();
    if (!context.valid() || localPeer == 0 || !prediction.loaded()) {
        return;
    }
    m_context = context;
    m_local = localPeer;
    m_prediction = &prediction;
    m_layout = &layout;
    m_scene = std::make_unique<WorldScene>(scene);
    m_collision = collision;
    m_worldMotion.bind(layout);
}

void OnlineOpponentReplay::clear() {
    m_context = {};
    m_local = 0;
    m_prediction = nullptr;
    m_layout = nullptr;
    m_scene.reset();
    m_frame.reset();
    m_contacts = {};
    m_worldState.reset();
    m_transforms.clear();
    m_shown.clear();
    m_previous.clear();
    m_baseline = m_end = 0;
    m_latestInput = {};
    m_instances = {};
    m_independentThrough = 0;
    m_committed = false;
}

bool OnlineOpponentReplay::eligible(const CombatSnapshot& authority) const {
    if (m_prediction == nullptr || m_scene == nullptr || m_layout == nullptr ||
        authority.motion.epoch != m_context.epoch || !authority.opponents ||
        !authority.opponentFrame || !authority.geometry || !authority.worldMotion ||
        !authority.valid()) {
        return false;
    }
    const auto& frame = *authority.opponentFrame;
    if (frame.held || frame.timeStopped || frame.unsupportedCombatants || frame.ticks != 1 ||
        m_context.tickRate != 60) {
        return false;
    }
    const auto& opponents = *authority.opponents;
    if (frame.pendingPlacements !=
        (opponents.placements ? opponents.placements->pending.size() : 0)) {
        return false;
    }
    if (opponents.generators.generators.empty() && opponents.missiles.missiles.empty() &&
        opponents.missiles.bursts.empty() &&
        (!opponents.placements || opponents.placements->pending.empty()) &&
        std::ranges::none_of(opponents.enemies.enemies, [](const auto& enemy) {
            return enemy.state != Enemies::State::Inactive;
        })) {
        // An empty island has no dynamic opponent contacts to contribute. In
        // particular, the Tower's unrelated moving scenery must not turn this
        // optional, stricter continuation into a veto on local player movement.
        return false;
    }
    usize seen = 0;
    bool local = false;
    for (usize seat = 0; seat < authority.players.size(); ++seat) {
        if (!authority.players[seat]) {
            continue;
        }
        ++seen;
        const auto found = std::ranges::find(frame.players, seat, &OpponentFrame::Player::seat);
        if (found == frame.players.end() || !authority.motion.players[seat] ||
            authority.motion.players[seat]->grant != m_context.grants[seat]) {
            return false;
        }
        const auto& player = *authority.players[seat];
        // Neutral new input does not cancel an already-started throw. Remote
        // wind-ups and concealed held attacks can also release during this
        // horizon. Until outgoing hits are replayed, none may enter the quiet
        // opponent phase merely because no new attack button was sampled.
        if (!player.rollback || !ordinaryAction(player.rollback->action) ||
            (player.rollbackInput &&
             (player.rollbackInput->heldButtons & ~static_cast<u32>(CommandHeld::Strafe)) != 0)) {
            return false;
        }
        if (m_context.owners[seat] != m_local) {
            continue;
        }
        local = true;
        if (!player.movement.enabled || !player.movement.current || player.movement.platform ||
            player.life != ReplicaPlayerLife::Standing || player.portalPhase || !player.rollback ||
            !ordinaryAction(player.rollback->action) || !player.locomotion ||
            player.locomotion->automatic || player.reaction != PlayerDeed::None ||
            (player.modifiers && player.modifiers->shields != 0)) {
            return false;
        }
    }
    std::vector<EnemyView> views;
    for (const auto& player : frame.players) {
        views.push_back(player.view);
    }
    OpponentSimulation::Frame phase;
    phase.ticks = frame.ticks;
    phase.seconds = frame.seconds;
    phase.players = views;
    phase.fixtures = frame.fixtures;
    phase.stops = frame.stops;
    phase.missileSpeed = frame.missileSpeed;
    return local && seen == frame.players.size() &&
           OpponentSimulation::supported(*authority.opponents, phase);
}

LocalMovementPrediction::ReplayCallbacks
OnlineOpponentReplay::callbacks(const CombatSnapshot* authority,
                                std::span<const InputCommand> sent) {
    // A moving opponent can enter melee-acquisition range during a held attack.
    // Until that selector is also replayed against the private pool, retain the
    // independent certified attack lane instead of reusing a stale ranged bit.
    constexpr u32 allowedHeld = static_cast<u32>(CommandHeld::Strafe);
    constexpr u32 allowedPress = static_cast<u32>(CommandPress::Defend);
    for (const auto& input : sent) {
        if (!input.valid() || input.epoch != m_context.epoch || m_local == 0 ||
            m_context.owners[input.seat] != m_local ||
            input.grant != m_context.grants[input.seat]) {
            continue;
        }
        m_latestInput[input.seat] = std::max(m_latestInput[input.seat], input.tick);
        if ((input.heldButtons & ~allowedHeld) != 0 ||
            (input.pressedButtons & ~allowedPress) != 0) {
            m_independentThrough = std::max(m_independentThrough, input.tick);
        }
    }
    if (authority == nullptr || !eligible(*authority)) {
        finish(false);
        return {};
    }
    std::optional<u64> horizon;
    bool aligned = true;
    for (usize seat = 0; seat < m_latestInput.size(); ++seat) {
        if (m_context.owners[seat] != m_local) {
            continue;
        }
        const u64 end = std::max(authority->motion.tick, m_latestInput[seat]);
        aligned = aligned && (!horizon || *horizon == end);
        horizon = end;
    }
    if (!aligned || m_independentThrough > authority->motion.tick ||
        (horizon && *horizon - authority->motion.tick > PlayerLocomotionState::kMaxTicks)) {
        // The independent replay already supports stationary throws at longer
        // leads and additional action lanes. This optional contact island must
        // never revoke those merely because its own bounded domain is narrower.
        finish(false);
        return {};
    }
    // Resource/roster mismatches are known before replay, not new interactions.
    // Do not let an optional seed disable already-supported player-only lanes.
    if (!restore(*authority)) {
        finish(false);
        return {};
    }
    return {
        .begin = [this](const CombatSnapshot& state) { return restore(state); },
        .beforeTick = [this](const auto& tick, auto& contacts) { return before(tick, contacts); },
        .afterTick = [this](const auto& tick) { return after(tick); },
        .end = [this](bool committed) { finish(committed); }};
}

bool OnlineOpponentReplay::restore(const CombatSnapshot& authority) {
    m_committed = false;
    m_shown.clear();
    m_previous.clear();
    if (!eligible(authority) || !m_worldMotion.prepare(*authority.worldMotion, *authority.geometry,
                                                       *m_scene, m_collision)) {
        return false;
    }
    m_frame = authority.opponentFrame;
    for (usize seat = 0; seat < m_contacts.size(); ++seat) {
        m_contacts[seat] =
            authority.players[seat] ? authority.players[seat]->locomotion : std::nullopt;
    }
    m_worldState = authority.worldMotion;
    m_transforms.clear();
    for (usize index = 0; index < m_layout->objects().size(); ++index) {
        m_transforms.push_back(m_scene->worldTransform(index));
    }
    m_baseline = m_end = authority.motion.tick;
    for (usize slot = 0; slot < m_instances.size(); ++slot) {
        const auto& enemy = authority.opponents->enemies.enemies[slot];
        m_instances[slot] = enemy.state != Enemies::State::Inactive ? enemy.instance : 0;
    }
    return m_prediction->begin(m_context.epoch, m_baseline, *authority.opponents, m_collision);
}

bool OnlineOpponentReplay::stationaryGeometry() const {
    if (!m_worldState || m_worldState->collision.size() != m_transforms.size()) {
        return false;
    }
    for (usize index = 0; index < m_transforms.size(); ++index) {
        const auto object = static_cast<s32>(index);
        const auto flags = static_cast<u8>(
            (m_collision.solid(object) ? WorldMotion::kSolid : 0) |
            (m_collision.contactOnly(object) ? WorldMotion::kContactOnly : 0) |
            (m_collision.floorExitBlocked(object) ? WorldMotion::kFloorExitBlocked : 0));
        // Do not capture animation, trigger and rotator pools twice per replay
        // tick merely to compare these three collision bits.
        if (flags != m_worldState->collision[index]) {
            return false;
        }
        if (m_collision.moving(object) && m_scene->worldTransform(index) != m_transforms[index]) {
            return false;
        }
    }
    return true;
}

bool OnlineOpponentReplay::before(const LocalMovementPrediction::ReplayTick& tick,
                                  LocalMovementPrediction::ContactOverlay& contacts) {
    if (!m_frame || !m_prediction->active() || tick.tick != m_prediction->tick() + 1 ||
        tick.tick - m_baseline > PlayerLocomotionState::kMaxTicks ||
        !m_worldMotion.beforeMotion(m_frame->seconds, *m_scene, m_collision) ||
        !stationaryGeometry()) {
        return false;
    }
    const auto* enemies = m_prediction->enemies();
    const auto* generators = m_prediction->generators();
    if (enemies == nullptr || generators == nullptr) {
        return false;
    }
    LocalMovementPrediction::DynamicContacts dynamic;
    for (const auto& body : enemies->targets()) {
        const auto observation = enemies->observe(body.id);
        if (!observation) {
            return false;
        }
        dynamic.swarm.push_back({body.base,
                                 body.radius,
                                 body.height,
                                 {PlayerLocomotionState::Identity::Kind::Swarm,
                                  static_cast<u32>(body.id), observation->instance}});
    }
    for (const auto& generator : generators->identifiedObstacles()) {
        dynamic.generators.push_back(
            {{PlayerLocomotionState::Identity::Kind::Generator, generator.slot, 0}, generator.box});
    }
    for (usize seat = 0; seat < tick.actors.size(); ++seat) {
        if (m_context.owners[seat] == m_local && m_local != 0) {
            const auto& actor = tick.actors[seat];
            if (actor.status != LocalMovementPrediction::ReplayStatus::Active ||
                actor.tick + 1 != tick.tick || !actor.animation ||
                !ordinaryAction(actor.animation->action)) {
                return false;
            }
            if (!m_contacts[seat]) {
                return false;
            }
            const auto captured =
                std::ranges::find(m_frame->players, seat, &OpponentFrame::Player::seat);
            if (captured == m_frame->players.end()) {
                return false;
            }
            auto nearby = dynamic;
            const auto outside = [&](const Vec3& position, f32 radius) {
                const Vec3 offset = position - m_contacts[seat]->origin;
                return std::hypot(offset.x, offset.z) >
                       m_contacts[seat]->radius + captured->view.radius + radius;
            };
            // Preserve the captured contact domain, not an arbitrary prefix of
            // the level roster. Distant generators must not consume a seat's
            // local contact budget; approaching bodies are sampled each tick.
            std::erase_if(nearby.swarm,
                          [&](const auto& body) { return outside(body.base, body.radius); });
            std::erase_if(nearby.generators, [&](const auto& generator) {
                const auto& box = generator.box;
                return !box.solid ||
                       outside(box.centre, std::max(box.cylinderRadius,
                                                    std::hypot(box.halfAcross, box.halfAlong)));
            });
            contacts.seats[seat] = std::move(nearby);
        }
    }
    return true;
}

bool OnlineOpponentReplay::after(const LocalMovementPrediction::ReplayTick& tick) {
    if (!m_frame || !m_prediction->active()) {
        return false;
    }
    std::vector<EnemyView> players;
    for (const auto& captured : m_frame->players) {
        auto view = captured.view;
        const auto& actor = tick.actors[captured.seat];
        if (m_context.owners[captured.seat] == m_local) {
            if ((actor.status != LocalMovementPrediction::ReplayStatus::Active &&
                 actor.status != LocalMovementPrediction::ReplayStatus::Complete) ||
                actor.tick != tick.tick || !actor.motion || !actor.animation ||
                !ordinaryAction(actor.animation->action)) {
                return false;
            }
            view.position = actor.motion->position;
            view.blockableAttack = PlayerAnimator::isBlockableAttack(actor.animation->action);
        }
        players.push_back(view);
    }
    OpponentSimulation::Frame frame;
    frame.ticks = m_frame->ticks;
    frame.seconds = m_frame->seconds;
    frame.players = players;
    frame.fixtures = m_frame->fixtures;
    frame.stops = m_frame->stops;
    frame.missileSpeed = m_frame->missileSpeed;
    m_previous = picture(*m_prediction, m_instances);
    if (m_prediction->advance(tick.tick, frame) != OpponentSimulation::Result::Advanced ||
        !m_worldMotion.afterMotion(m_frame->seconds, *m_scene, m_collision) ||
        !stationaryGeometry()) {
        return false;
    }
    m_end = tick.tick;
    return true;
}

void OnlineOpponentReplay::finish(bool committed) {
    m_committed = committed && m_prediction != nullptr && m_prediction->active() &&
                  m_prediction->tick() == m_end && m_end > m_baseline;
    m_shown = m_committed ? picture(*m_prediction, m_instances) : std::vector<EnemyCombatState>{};
    if (!m_committed && m_prediction != nullptr) {
        m_prediction->resetReplay();
        m_previous.clear();
    }
}

bool OnlineOpponentReplay::active() const {
    return m_committed && m_prediction != nullptr && m_prediction->active();
}

void OnlineOpponentReplay::present(CombatSnapshot& shown, EnemyPresentation& presentation,
                                   f32 blend) const {
    if (!active() || shown.motion.epoch != m_context.epoch || shown.motion.tick > m_baseline ||
        !presentation.matches(shown) || !std::isfinite(blend)) {
        return;
    }
    blend = std::clamp(blend, 0.0f, 1.0f);
    for (auto& enemy : shown.enemies) {
        const auto now =
            std::ranges::lower_bound(m_shown, enemy.instance, {}, &EnemyCombatState::instance);
        if (now == m_shown.end() || now->instance != enemy.instance || now->kind != enemy.kind ||
            now->tier != enemy.tier || now->variant != enemy.variant || now->life != enemy.life) {
            continue;
        }
        auto pose = *now;
        const auto previous =
            std::ranges::lower_bound(m_previous, enemy.instance, {}, &EnemyCombatState::instance);
        if (previous != m_previous.end() && previous->instance == enemy.instance &&
            previous->life == pose.life && previous->animation.action == pose.animation.action &&
            previous->animation.generation == pose.animation.generation &&
            glm::distance(previous->position, pose.position) < 32) {
            pose.position = Enemies::supportedPresentation(
                m_prediction->collision(), previous->position, pose.position, blend,
                static_cast<EnemyAction>(pose.animation.action),
                pose.life == ReplicaEnemyLife::Active);
            pose.yaw = previous->yaw + std::remainder(pose.yaw - previous->yaw, kTwoPi) * blend;
            if (pose.animation.frame >= previous->animation.frame) {
                pose.animation.frame =
                    std::lerp(previous->animation.frame, pose.animation.frame, blend);
            }
        }
        // Do not replace authoritative health, lifetimes or ownership. Only the
        // known body's native position/animation/fade is presented early.
        enemy.position = pose.position;
        enemy.yaw = pose.yaw;
        enemy.animation = pose.animation;
        enemy.alpha = pose.alpha;
        enemy.hitFlash = pose.hitFlash;
        enemy.deathFrame = pose.deathFrame;
        const auto interval =
            std::ranges::lower_bound(presentation.enemies, enemy.instance, {},
                                     [](const auto& row) { return row.from.instance; });
        if (interval != presentation.enemies.end() && interval->from.instance == enemy.instance) {
            *interval = {enemy, enemy};
        }
    }
}
} // namespace gdl::game
