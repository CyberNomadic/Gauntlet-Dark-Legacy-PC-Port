#include "game/screens/LocalMovementPrediction.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "game/combat/MeleeGeometry.h"
#include "game/players/CursorAim.h"
#include "game/players/MeleeSelection.h"
#include "game/players/PlayerActor.h"
#include "game/players/PlayerAnimator.h"
#include "game/players/PowerupEffects.h"
#include "game/players/TurboMove.h"
#include "game/screens/FireShieldPresentation.h"
#include "game/screens/FloorRiding.h"
#include "game/screens/PartyCollision.h"
#include "game/screens/PartyMotion.h"
#include "game/screens/PlayerAttacks.h"
#include "game/screens/SessionInputs.h"
#include "game/world/BodyCollision.h"
#include "game/world/CameraMovementLimit.h"
#include "game/world/WorldMotion.h"

namespace gdl::game {
namespace {
bool rapidFire(const PlayerCombatState& state) {
    return state.modifiers && (state.modifiers->flags & powerup::kRapidFire) != 0;
}

bool speedBoost(const PlayerCombatState& state) {
    return state.modifiers && state.modifiers->speed;
}

bool shielded(const PlayerCombatState& state) {
    return state.modifiers && state.modifiers->shields != 0;
}

u32 weaponFlags(const PlayerCombatState& state) {
    return state.modifiers ? state.modifiers->flags : 0;
}

u8 weaponShots(const PlayerCombatState& state) {
    PowerupEffects effects;
    effects.weapon = weaponFlags(state);
    return static_cast<u8>(effects.shots());
}

/** The native row/meter timeline with observation-only event sinks. This also
 * advances the meter through ordinary actions and recovery after a turbo ends. */
struct ReplayedTurbo {
    TurboMeter meter;
    TurboMove move;
    const ClassStats* stats = nullptr;
    std::vector<TurboMove::Volley> volleys;
    std::vector<TurboMove::Strike> strikes;

    void advance(const PlayerAnimator& animator, const Vec3& facing, f32 seconds) {
        volleys.clear();
        strikes.clear();
        if (animator.turboBegan()) {
            move.begin(animator.action(), stats, meter);
        }
        move.advance(
            animator.action(), animator.player().frame(), facing, stats, meter,
            {.announce = [](s32) {},
             .dim = [](f32) {},
             .identifiedVolley = [this](const auto& volley) { volleys.push_back(volley); },
             .identifiedStrike = [this](const auto& strike) { strikes.push_back(strike); }});
        if (animator.shoving()) {
            meter.drain(seconds);
        } else if (!animator.turboing()) {
            meter.fill(seconds);
        }
        meter.step(1);
    }

    PlayerTurboState checkpoint() const { return {meter.checkpoint(), move.checkpoint()}; }
};

void recordFrame(LocalMovementPrediction::Replay& replay, u64 tick, const SeatMotion& base,
                 const PlayerActor& actor, const PlayerAnimator& animator,
                 const std::optional<ReplayedTurbo>& turbo) {
    LocalMovementPrediction::Replay::Frame frame;
    frame.tick = tick;
    frame.motion = base;
    frame.motion.position = actor.position();
    frame.motion.yaw = std::remainder(actor.yaw(), kTwoPi);
    frame.animation = animator.checkpoint();
    frame.transitionPose = animator.transitionPose();
    if (turbo) {
        frame.strikes = turbo->strikes;
    }
    replay.frames.push_back(std::move(frame));
}

std::optional<ReplayedTurbo> restoreTurbo(const PlayerCombatState& state, const ClassStats* stats,
                                          s32 character, const PlayerAnimator& animator) {
    const s32 baseCharacter =
        character < kSumnerClass ? character % kStartingClassCount : character;
    ReplayedTurbo result;
    result.stats = stats;
    if (!state.turbo || !state.rollback || stats == nullptr || character < 0 ||
        state.rollback->character != baseCharacter || !result.meter.restore(state.turbo->meter) ||
        !result.move.restore(state.turbo->move, stats, animator.action())) {
        return std::nullopt;
    }
    return result;
}
} // namespace

/** One restored native actor and its lane-specific clocks. Preparation is once
 * per coordinated pass; step advances only the next retained command. Nothing
 * is published until every requested step for this seat has succeeded. */
struct LocalMovementPrediction::Continuation {
    enum class Lane { Ordinary, Bounded, Reaction };
    Continuation(LocalMovementPrediction& owner, usize seatIndex,
                 std::span<const RollbackInputHistory::Entry> entries)
        : prediction(owner), index(seatIndex), seat(owner.m_seats[index]),
          state(*owner.m_authority->players[index]),
          motion(*owner.m_authority->motion.players[index]), envelope(state.movement),
          commands(entries), m_authority(owner.m_authority), m_collision(owner.m_collision),
          m_context(owner.m_context), m_layout(owner.m_layout), m_geometry(owner.m_geometry),
          seconds(1.0f / static_cast<f32>(m_context.tickRate)) {}

    bool prepare();
    bool prepareBounded();
    bool prepareKnockback();
    bool step();
    bool stepOrdinary(const RollbackInputHistory::Entry& command);
    bool stepBounded(const RollbackInputHistory::Entry& command);
    bool stepKnockback(const RollbackInputHistory::Entry& command);
    void finish();
    void finishOrdinary();
    void finishBounded();
    void finishKnockback();
    void authoritative() { prediction.authoritative(index); }
    bool pending() const { return !failed && next < commands.size(); }
    u64 nextTick() const { return commands[next].input.tick; }
    const WorldCollision& collision() const {
        return worldCollision ? *worldCollision : m_collision;
    }
    bool retainSupport(s32 object);
    bool staticGrounded() const;
    bool resolveContacts(const Vec3& before);
    bool stationaryClear();
    void resetContacts();
    bool replaceContacts(const DynamicContacts& dynamic);
    bool boundedGrounded() const;
    bool fixturesClear() const;
    bool clearStep(const Vec3& before) const;
    MeleeSelection::Snapshot acquisition(const Vec3& from, const Vec3& to) const;
    bool reactionGrounded() const;
    ReplayActor observation() const;

    LocalMovementPrediction& prediction;
    usize index;
    Seat& seat;
    const PlayerCombatState& state;
    const SeatMotion& motion;
    const PlayerMovementState& envelope;
    std::span<const RollbackInputHistory::Entry> commands;
    const std::optional<CombatSnapshot>& m_authority;
    const WorldCollision& m_collision;
    const MatchContext& m_context;
    const WorldLayout* m_layout;
    const std::unique_ptr<WorldScene>& m_geometry;
    const f32 seconds;
    Lane lane = Lane::Ordinary;
    usize next = 0;
    bool failed = false;
    bool fallback = true;
    std::array<PlayerRuntime, 1> platformParty;
    PlayerRuntime& rider = platformParty.front();
    PlayerActor& actor = rider.actor;
    PlayerAnimator animator;
    bool shieldLocomotion = false;
    bool onlyLocomotion = false;
    bool potionReplay = false;
    bool turboReplay = false;
    std::optional<PotionSimulation> potion;
    std::optional<ReplayedTurbo> turbo;
    Knockback idleImpulse;
    Knockback impulse;
    Knockback knockback;
    PlayerDeed reaction = PlayerDeed::None;
    const PlayerMeleeState* geometry = nullptr;
    FireShieldPresentation fireShield;
    bool replayFire = false;
    f32 fireSeconds = 0;
    WorldMotion worldMotion;
    std::optional<WorldScene> worldScene;
    std::optional<WorldCollision> worldCollision;
    std::vector<u32> carriedObjects;
    s32 certifiedAncestor = -1;
    const PlayerLocomotionState* contacts = nullptr;
    bool stationaryAction = false;
    bool staticTerrain = false;
    std::vector<MissileTarget> bodies;
    std::optional<PlayerLocomotionState> contactOverlay;
    std::vector<PlayerRuntime> contactParty;
    Replay replay;
};

void LocalMovementPrediction::begin(const MatchContext& context, u8 localPeer,
                                    const WorldCollision& collision, const WorldScene* scene,
                                    const WorldLayout* layout) {
    clear();
    m_context = context;
    m_local = localPeer;
    RollbackInputHistory::Grants grants{};
    for (usize i = 0; i < grants.size(); ++i) {
        if (localPeer != 0 && context.owners[i] == localPeer) {
            grants[i] = context.grants[i];
        }
    }
    m_history.begin(context.epoch, grants);
    m_collision = collision;
    m_layout = layout;
    if (scene != nullptr && layout != nullptr) {
        m_geometry = std::make_unique<WorldScene>(*scene);
    }
}
void LocalMovementPrediction::clear() {
    *this = LocalMovementPrediction{};
}
bool LocalMovementPrediction::setPlayer(u8 seat, const TreeInfo& tree, const ClassStats* stats,
                                        s32 character) {
    if (seat >= m_seats.size() || m_context.owners[seat] != m_local || m_local == 0) {
        return false;
    }
    if ((stats != nullptr && (character < 0 || character >= kClassCount)) ||
        (stats == nullptr && character != -1)) {
        return false;
    }
    PlayerAnimator check;
    if (!check.bind(tree, false)) {
        return false;
    }
    m_seats[seat].tree = &tree;
    m_seats[seat].stats = stats;
    m_seats[seat].character = character;
    return true;
}
bool LocalMovementPrediction::setFireShield(u8 seat, const TreeInfo& tree, f32 bodyScale) {
    FireShieldPresentation check;
    if (seat >= m_seats.size() || m_context.owners[seat] != m_local || m_local == 0 ||
        !std::isfinite(bodyScale) || bodyScale <= 0 || !check.restore(tree, std::nullopt)) {
        return false;
    }
    m_seats[seat].fireShieldTree = &tree;
    m_seats[seat].bodyScale = bodyScale;
    return true;
}
const LocalMovementPrediction::Replay* LocalMovementPrediction::replayed(u8 seat) const {
    if (seat >= m_seats.size() || !m_seats[seat].replay) {
        return nullptr;
    }
    return &*m_seats[seat].replay;
}
bool LocalMovementPrediction::supported(const InputCommand& input, const PlayerCombatState& state,
                                        bool turboReplay, bool potionReplay) {
    if (shielded(state) || state.movement.platform) {
        constexpr u32 kActionsHeld =
            static_cast<u32>(CommandHeld::Attack) | static_cast<u32>(CommandHeld::StrongAttack) |
            static_cast<u32>(CommandHeld::ShieldPotion) | static_cast<u32>(CommandHeld::Combo) |
            static_cast<u32>(CommandHeld::Turbo) | static_cast<u32>(CommandHeld::ThrowPotion);
        constexpr u32 kActionsPressed =
            static_cast<u32>(CommandPress::Defend) | static_cast<u32>(CommandPress::TurboAttack) |
            static_cast<u32>(CommandPress::Charge) | static_cast<u32>(CommandPress::UsePotion) |
            static_cast<u32>(CommandPress::Attack) | static_cast<u32>(CommandPress::SelectorUp) |
            static_cast<u32>(CommandPress::SelectorDown) |
            static_cast<u32>(CommandPress::SelectorLeft) |
            static_cast<u32>(CommandPress::SelectorRight);
        // Shield and platform certificates admit only native locomotion. Even a
        // contradictory ranged/melee/turbo capability cannot authorize attacks.
        return (input.heldButtons & kActionsHeld) == 0 &&
               (input.pressedButtons & kActionsPressed) == 0;
    }
    if (potionReplay) {
        constexpr u32 kHeld =
            static_cast<u32>(CommandHeld::Attack) | static_cast<u32>(CommandHeld::StrongAttack) |
            static_cast<u32>(CommandHeld::ShieldPotion) | static_cast<u32>(CommandHeld::Combo) |
            static_cast<u32>(CommandHeld::Turbo);
        constexpr u32 kPressed =
            static_cast<u32>(CommandPress::Attack) | static_cast<u32>(CommandPress::Defend) |
            static_cast<u32>(CommandPress::Charge) | static_cast<u32>(CommandPress::TurboAttack) |
            static_cast<u32>(CommandPress::SelectorUp) |
            static_cast<u32>(CommandPress::SelectorDown) |
            static_cast<u32>(CommandPress::SelectorLeft) |
            static_cast<u32>(CommandPress::SelectorRight);
        // Mixed attack inputs also refresh native melee caches even when potion
        // priority wins. Keep those cross-action paths authoritative until that
        // acquisition is represented, rather than substituting a different deed.
        return state.potion && state.rollback && (input.heldButtons & kHeld) == 0 &&
               (input.pressedButtons & kPressed) == 0;
    }
    constexpr u32 kUnsupportedHeld = static_cast<u32>(CommandHeld::ShieldPotion) |
                                     static_cast<u32>(CommandHeld::Combo) |
                                     static_cast<u32>(CommandHeld::ThrowPotion);
    constexpr u32 kUnsupportedPress = static_cast<u32>(CommandPress::UsePotion) |
                                      static_cast<u32>(CommandPress::SelectorUp) |
                                      static_cast<u32>(CommandPress::SelectorDown) |
                                      static_cast<u32>(CommandPress::SelectorLeft) |
                                      static_cast<u32>(CommandPress::SelectorRight);
    if ((input.heldButtons & kUnsupportedHeld) != 0 ||
        (input.pressedButtons & kUnsupportedPress) != 0 ||
        (input.pressed(CommandPress::Charge) && !state.shove) ||
        ((input.pressed(CommandPress::TurboAttack) || input.pressed(CommandPress::Charge)) &&
         (!state.turbo || !state.turboGeometry || !state.turboGeometry->moving))) {
        return false;
    }
    if (input.pressed(CommandPress::Defend) && !state.movement.ranged && !state.melee &&
        !state.turbo) {
        // Plain locomotion can stay available with unsupported equipment. It is
        // not permission to raise a guard with an unmodeled item/armor effect.
        return false;
    }
    const bool attack = input.held(CommandHeld::Attack) || input.held(CommandHeld::StrongAttack);
    // A visible animation cursor alone cannot restore the native input latches,
    // shot cadence or cached action speed. Older motion-only fixtures may still
    // move, but must never invent an attack from that display-only state.
    if (attack || input.pressed(CommandPress::Defend)) {
        return state.rollback && (!attack || state.movement.ranged ||
                                  (turboReplay && state.turbo && state.turboGeometry &&
                                   state.turboGeometry->moving) ||
                                  (state.melee && (state.melee->moving ||
                                                   !SessionInputs::playInput(input).move.any())));
    }
    return true;
}
void LocalMovementPrediction::update(const CombatSnapshot* authority,
                                     std::span<const InputCommand> sent,
                                     const ReplayCallbacks& callbacks, bool simulate) {
    ++m_clock;
    for (auto& seat : m_seats) {
        seat.previous = seat.pose;
        seat.previousAnimation = seat.animation;
        seat.previousCorrection = seat.correction;
        seat.correction *= 0.75f;
    }
    const bool fresh = authority != nullptr && authority->motion.epoch == m_context.epoch &&
                       (!m_authority || authority->motion.tick > m_authority->motion.tick);
    if (fresh) {
        if (!authority->valid() ||
            (m_geometry &&
             (!authority->geometry || !m_geometry->applyGeometry(*authority->geometry)))) {
            m_authority.reset();
            for (usize i = 0; i < m_seats.size(); ++i) {
                auto& seat = m_seats[i];
                seat.pose.reset();
                seat.previous.reset();
                seat.replay.reset();
                m_history.invalidate(i);
                seat.correction = Vec3{0};
                seat.previousCorrection = Vec3{0};
            }
            m_metrics.pending = 0;
            return;
        }
        if (m_geometry) {
            const auto& objects = m_layout->objects();
            for (usize i = 0; i < objects.size(); ++i) {
                const auto id = static_cast<s32>(i);
                m_collision.setObjectTransform(id, m_geometry->worldTransform(i));
                if (authority->worldMotion) {
                    const u8 flags = authority->worldMotion->collision[i];
                    m_collision.setSolid(id, (flags & WorldMotion::kSolid) != 0);
                    m_collision.setContactOnly(id, (flags & WorldMotion::kContactOnly) != 0);
                    m_collision.setFloorExitBlocked(id,
                                                    (flags & WorldMotion::kFloorExitBlocked) != 0);
                } else {
                    m_collision.setSolid(id, m_geometry->objectVisible(i));
                }
            }
        }
        for (usize i = 0; i < m_seats.size(); ++i) {
            auto& seat = m_seats[i];
            const auto& motion = authority->motion.players[i];
            const auto& state = authority->players[i];
            const bool cut = !motion || !state || !state->movement.enabled ||
                             state->life != ReplicaPlayerLife::Standing || state->portalPhase ||
                             motion->grant != m_context.grants[i] || m_history.resyncRequired(i) ||
                             (m_authority && m_authority->motion.players[i] &&
                              motion->continuity != m_authority->motion.players[i]->continuity);
            for (const auto& command : m_history.replay(i)) {
                if (!command.predicted && command.input.tick <= authority->motion.tick) {
                    m_metrics.resolvedDeadlineMs = static_cast<f32>(m_clock - command.sampled) *
                                                   1000 / static_cast<f32>(m_context.tickRate);
                }
            }
            if (cut) {
                if (seat.pose) {
                    ++m_metrics.cuts;
                }
                seat.pose.reset();
                seat.previous.reset();
                m_history.invalidate(i);
                seat.correction = Vec3{0};
                seat.previousCorrection = Vec3{0};
            }
        }
        // A completed host step resolves even commands that arrived too late.
        // Receipt watermarks must not keep a rejected local action alive.
        m_history.resolve(authority->motion.epoch, authority->motion.tick);
        m_authority = *authority;
        m_receivedAt = m_clock;
        // Compare the same set of sampled commands on either side of a host
        // correction. New movement sampled below is not reconciliation error.
        std::array<std::optional<SeatMotion>, InputCommand::kSeats> beforePoses;
        std::array<bool, InputCommand::kSeats> authoritativeBefore{};
        for (usize i = 0; i < m_seats.size(); ++i) {
            beforePoses[i] = m_seats[i].pose;
            authoritativeBefore[i] = m_seats[i].authoritativeOnly;
        }
        if (simulate) {
            replayAll(callbacks);
        }
        for (usize i = 0; i < m_seats.size(); ++i) {
            auto& seat = m_seats[i];
            const auto& before = beforePoses[i];
            const bool previouslyAuthoritative = authoritativeBefore[i];
            if (before && seat.pose && !previouslyAuthoritative && !seat.authoritativeOnly &&
                !seat.geometry && before->continuity == seat.pose->continuity) {
                const f32 correction = glm::distance(before->position, seat.pose->position);
                if (correction > 0.01f) {
                    ++m_metrics.corrections;
                    m_metrics.largestCorrection = std::max(m_metrics.largestCorrection, correction);
                    const Vec3 delta = before->position - seat.pose->position;
                    // Ease small horizontal corrections over a few ticks. Height,
                    // large errors and discontinuities always snap to the host.
                    const Vec3 offset = seat.correction + Vec3{delta.x, 0, delta.z};
                    seat.correction =
                        std::abs(delta.y) < 0.1f && glm::length(offset) < 0.75f ? offset : Vec3{0};
                }
            }
        }
    }
    for (const auto& input : sent) {
        if (!input.valid() || input.epoch != m_context.epoch || m_local == 0 ||
            m_context.owners[input.seat] != m_local ||
            input.grant != m_context.grants[input.seat] ||
            (m_authority &&
             (input.tick <= m_authority->motion.tick || !m_authority->players[input.seat] ||
              !m_authority->players[input.seat]->movement.enabled))) {
            continue;
        }
        const auto before = m_history.count(input.seat);
        if (m_history.record(std::span{&input, 1}, m_clock)) {
            m_metrics.samples += m_history.count(input.seat) - before;
        }
    }
    m_metrics.pending = m_history.pending();
    if (simulate) {
        replayAll(callbacks);
    } else {
        // A complete native combat pass already advanced these actors. Keep
        // acknowledged input history for fallback without running them twice.
        for (usize i = 0; i < m_seats.size(); ++i) {
            authoritative(i);
        }
    }
    for (usize i = 0; i < m_seats.size(); ++i) {
        auto& seat = m_seats[i];
        if (!seat.pose || seat.authoritativeOnly) {
            seat.correction = Vec3{0};
            seat.previousCorrection = Vec3{0};
            seat.previous.reset();
        }
        if (!seat.previous) {
            seat.previous = seat.pose;
        }
    }
}

void LocalMovementPrediction::replayAll(const ReplayCallbacks& callbacks) {
    std::array<std::unique_ptr<Continuation>, InputCommand::kSeats> actors;
    if (!m_authority) {
        for (usize index = 0; index < actors.size(); ++index) {
            prepare(index);
        }
        return;
    }
    const auto abort = [&] {
        for (usize index = 0; index < m_seats.size(); ++index) {
            if (m_local != 0 && m_context.owners[index] == m_local) {
                authoritative(index);
                m_seats[index].correction = Vec3{0};
                m_seats[index].previousCorrection = Vec3{0};
                m_seats[index].previous.reset();
            }
        }
        if (callbacks.end) {
            callbacks.end(false);
        }
    };
    if (callbacks.begin && !callbacks.begin(*m_authority)) {
        abort();
        return;
    }
    for (usize index = 0; index < actors.size(); ++index) {
        actors[index] = prepare(index);
    }
    const auto observe = [&](u64 tick) {
        ReplayTick result;
        result.tick = tick;
        for (usize index = 0; index < actors.size(); ++index) {
            auto& shown = result.actors[index];
            if (actors[index]) {
                shown = actors[index]->observation();
            } else if (m_authority->motion.players[index] && m_authority->players[index]) {
                shown.status = m_context.owners[index] == m_local && m_local != 0
                                   ? ReplayStatus::Unsupported
                                   : ReplayStatus::Remote;
                shown.tick = m_authority->motion.tick;
                shown.motion = m_authority->motion.players[index];
                shown.animation = m_authority->players[index]->rollback;
            }
        }
        return result;
    };
    // Each continuation retains its full native state across ticks. A shorter
    // local timeline stops at its actual last sampled command: it is not padded
    // to another seat's horizon, nor rerun from authority for each prefix.
    while (true) {
        std::optional<u64> tick;
        for (const auto& actor : actors) {
            if (actor && actor->pending() && (!tick || actor->nextTick() < *tick)) {
                tick = actor->nextTick();
            }
        }
        if (!tick) {
            break;
        }
        if (callbacks.beforeTick) {
            for (auto& actor : actors) {
                if (actor && actor->pending() && actor->nextTick() == *tick) {
                    actor->resetContacts();
                }
            }
            ContactOverlay overlay;
            overlay.epoch = m_context.epoch;
            overlay.tick = *tick;
            if (!callbacks.beforeTick(observe(*tick), overlay) ||
                overlay.epoch != m_context.epoch || overlay.tick != *tick) {
                abort();
                return;
            }
            for (usize index = 0; index < actors.size(); ++index) {
                if (overlay.seats[index] &&
                    (!actors[index] || !actors[index]->pending() ||
                     actors[index]->nextTick() != *tick ||
                     !actors[index]->replaceContacts(*overlay.seats[index]))) {
                    abort();
                    return;
                }
            }
        }
        for (auto& actor : actors) {
            if (actor && actor->pending() && actor->nextTick() == *tick) {
                actor->step();
            }
        }
        if (callbacks.afterTick) {
            // This is the native opponent-phase seam: all local actors and their
            // post-motion fixture corrections for this tick are complete. No
            // future local actor step runs until the private world accepts it.
            if (!callbacks.afterTick(observe(*tick))) {
                abort();
                return;
            }
        }
    }
    for (auto& actor : actors) {
        if (actor) {
            actor->finish();
        }
    }
    std::optional<u64> geometryTick;
    for (const auto& seat : m_seats) {
        if (seat.geometry && seat.replay) {
            geometryTick = seat.replay->endTick;
            break;
        }
    }
    if (geometryTick && std::ranges::any_of(m_seats, [&](const auto& seat) {
            return seat.replay && seat.replay->endTick != *geometryTick;
        })) {
        if (callbacks.begin || callbacks.beforeTick || callbacks.afterTick || callbacks.end) {
            // A coupled private world cannot commit a platform clock that local
            // actor presentation subsequently rejects.
            abort();
            return;
        }
        // A single view cannot show the same platform at two locally predicted
        // times. Retain newest authority until the owned input timelines align.
        for (usize index = 0; index < m_seats.size(); ++index) {
            auto& seat = m_seats[index];
            if (!seat.geometry) {
                continue;
            }
            const auto& state = *m_authority->players[index];
            seat.pose = m_authority->motion.players[index];
            seat.animation = state.animation;
            seat.checkpoint = state.rollback;
            seat.transitionPose = state.transitionPose;
            seat.knockback = state.knockback;
            seat.turbo = state.turbo;
            seat.floor = state.floor;
            seat.reaction = state.reaction;
            seat.geometry.reset();
            seat.geometryObjects.clear();
            seat.shadow.reset();
            seat.replay.reset();
            seat.authoritativeOnly = true;
            seat.companionReplayed = false;
            seat.correction = seat.previousCorrection = Vec3{0};
            seat.previous.reset();
        }
    }
    if (callbacks.end) {
        callbacks.end(true);
    }
}

std::unique_ptr<LocalMovementPrediction::Continuation>
LocalMovementPrediction::prepare(usize index) {
    auto& seat = m_seats[index];
    seat.pose.reset();
    seat.checkpoint.reset();
    seat.transitionPose.reset();
    seat.knockback.reset();
    seat.floor.reset();
    seat.geometry.reset();
    seat.geometryObjects.clear();
    seat.shadow.reset();
    seat.turbo.reset();
    seat.companionReplayed = false;
    seat.fireShield.reset();
    seat.reaction = PlayerDeed::None;
    seat.replay.reset();
    seat.authoritativeOnly = false;
    if (!m_authority || m_context.owners[index] != m_local || !m_authority->players[index] ||
        !m_authority->motion.players[index]) {
        return nullptr;
    }
    if (m_clock - m_receivedAt > kStaleTicks || m_history.resyncRequired(index)) {
        ++m_metrics.stalled;
        return nullptr;
    }
    const auto& state = *m_authority->players[index];
    const auto commands = m_history.replay(index, state.rollbackInput);
    if (!currentAuthority(index)) {
        return nullptr;
    }

    auto continuation = std::make_unique<Continuation>(*this, index, commands);
    if (!continuation->prepare()) {
        return nullptr;
    }
    return continuation;
}
bool LocalMovementPrediction::currentAuthority(usize index) const {
    if (!m_authority || m_local == 0 || m_context.owners[index] != m_local ||
        !m_authority->players[index] || !m_authority->motion.players[index] ||
        m_clock - m_receivedAt > kStaleTicks || m_history.resyncRequired(index)) {
        return false;
    }
    const auto& state = *m_authority->players[index];
    const auto& motion = *m_authority->motion.players[index];
    if ((!state.movement.enabled && !state.movement.current) || state.portalPhase ||
        state.life != ReplicaPlayerLife::Standing || motion.grant != m_context.grants[index]) {
        return false;
    }
    // Both replay and newest-authority fallback must respect the same support
    // boundary. Feet may not move independently of an uncertified moving floor.
    const auto floor = m_collision.floorAt(motion.position, 0.25f, 0.25f);
    return !floor || !m_collision.moving(floor->object) || state.movement.platform;
}
void LocalMovementPrediction::authoritative(usize index) {
    auto& seat = m_seats[index];
    seat.pose.reset();
    seat.checkpoint.reset();
    seat.transitionPose.reset();
    seat.knockback.reset();
    seat.floor.reset();
    seat.geometry.reset();
    seat.geometryObjects.clear();
    seat.shadow.reset();
    seat.turbo.reset();
    seat.companionReplayed = false;
    seat.fireShield.reset();
    seat.reaction = PlayerDeed::None;
    seat.replay.reset();
    seat.authoritativeOnly = false;

    if (!currentAuthority(index)) {
        return;
    }
    const auto& state = *m_authority->players[index];
    seat.pose = m_authority->motion.players[index];
    seat.animation = state.animation;
    seat.checkpoint = state.rollback;
    seat.transitionPose = state.transitionPose;
    seat.knockback = state.knockback;
    seat.floor = state.floor;
    seat.turbo = state.turbo;
    seat.reaction = state.reaction;
    seat.authoritativeOnly = true;
}
bool LocalMovementPrediction::Continuation::prepare() {
    actor.restoreMotion(motion.position, motion.yaw, envelope.speed, envelope.radius,
                        envelope.height, envelope.followHeight);
    if (!envelope.enabled) {
        // The host explicitly allows current local presentation, but this action
        // has world state we cannot replay. A confirmed melee/item action must not
        // jump back to the remote buffer after its pending input used this path.
        authoritative();
        return false;
    }
    if (state.modifiers && !commands.empty() &&
        commands.back().input.tick - m_authority->motion.tick > state.modifiers->ticks) {
        // The host checked its post-step inventory under the native pre-motion
        // expiry clock. Never extend the current flags or effective movement pace
        // across that certificate, including an overlapping Speed slot expiring.
        authoritative();
        return false;
    }
    if (seat.tree == nullptr || !animator.bind(*seat.tree, false)) {
        return false;
    }
    if (state.rollback ? !animator.restore(*state.rollback, state.transitionPose)
                       : !animator.restoreLocomotion(
                             static_cast<PlayerAnimator::Action>(state.animation.action),
                             state.animation.sequence, state.animation.frame)) {
        return false;
    }
    const bool reactionActive =
        state.knockback && (state.knockbackTicks != 0 || state.reaction != PlayerDeed::None ||
                            PlayerAnimator::rollbackReactionAction(animator.action()) ||
                            state.knockback->force != Vec3{0} || state.knockback->damage != 0 ||
                            state.knockback->flags != 0 || state.knockback->velocity != Vec3{0} ||
                            state.knockback->shove != Vec3{0} || state.knockback->shoved ||
                            state.knockback->pushed || state.knockback->fastSeconds != 0);
    shieldLocomotion = shielded(state);
    onlyLocomotion = shieldLocomotion || envelope.platform;
    if (onlyLocomotion && (reactionActive || !PlayerAnimator::locomotion(animator.action()) ||
                           std::ranges::any_of(commands, [this](const auto& command) {
                               return !supported(command.input, state);
                           }))) {
        authoritative();
        return false;
    }
    if (reactionActive) {
        lane = Lane::Reaction;
        if (!prepareKnockback()) {
            authoritative();
            return false;
        }
        return true;
    }
    potionReplay = animator.conjuring() || std::ranges::any_of(commands, [](const auto& command) {
                       return command.input.held(CommandHeld::ThrowPotion) ||
                              command.input.pressed(CommandPress::UsePotion);
                   });
    if (potionReplay) {
        if (onlyLocomotion || m_context.tickRate != 60 || !state.potion || !state.rollback ||
            !state.locomotion || !state.floor || state.reaction != PlayerDeed::None ||
            !PlayerPotionState::supports(*state.rollback) ||
            commands.size() > state.potion->ticks) {
            authoritative();
            return false;
        }
        PotionSimulation::Context powers;
        powers.powers = state.potion->powers;
        powers.bonuses = state.potion->bonuses;
        powers.damageFlags = state.potion->damageFlags;
        powers.rapid = rapidFire(state);
        powers.speed = speedBoost(state);
        PotionSimulation::Checkpoint baseline{
            m_authority->motion.tick, *state.rollback, state.transitionPose,
            state.potion->potions,    powers,          PlayerDeed::None,
            state.potion->ticks};
        potion.emplace();
        if (!potion->restore(*seat.tree, {m_context.epoch, motion.grant, static_cast<u8>(index)},
                             baseline)) {
            authoritative();
            return false;
        }
    }
    const bool turboRequested = std::ranges::any_of(commands, [](const auto& command) {
        return command.input.pressed(CommandPress::TurboAttack) ||
               command.input.pressed(CommandPress::Charge);
    });
    if (!onlyLocomotion &&
        (PlayerAnimator::rollbackTurboAction(animator.action()) ||
         PlayerAnimator::rollbackShoveAction(animator.action()) || turboRequested)) {
        lane = Lane::Bounded;
        turboReplay = true;
        geometry = state.turboGeometry ? &*state.turboGeometry : nullptr;
        if ((PlayerAnimator::rollbackShoveAction(animator.action()) && !state.shove) ||
            !state.turbo || geometry == nullptr || !prepareBounded()) {
            authoritative();
            return false;
        }
        return true;
    }
    if (!onlyLocomotion && !potionReplay && state.melee && state.melee->moving) {
        lane = Lane::Bounded;
        geometry = &*state.melee;
        if (!prepareBounded()) {
            authoritative();
            return false;
        }
        return true;
    }
    if (!envelope.platform && state.floor && !state.locomotion) {
        // Native captures always include floor ownership. An independent turbo
        // or stationary-melee certificate can keep this seat enabled, but does
        // not certify the ordinary walking/throwing branch's dynamic contacts.
        authoritative();
        return false;
    }
    // Every native player has an impulse clock, including an otherwise empty
    // checkpoint halfway through its 30 Hz phase. That clock is not a reaction
    // and must not revoke ordinary movement, melee or weapon prediction.
    if (state.knockback && !idleImpulse.restore(*state.knockback)) {
        authoritative();
        return false;
    }
    if (!PlayerAnimator::rollbackAction(animator.action()) &&
        !(state.melee && PlayerAnimator::rollbackMeleeAction(animator.action())) && !potionReplay) {
        return false;
    }
    if (std::ranges::any_of(commands, [this](const auto& command) {
            return !supported(command.input, state, false, potionReplay);
        })) {
        // An attack near a target can become melee or an item action, with a
        // different movement pace. Do not invent either the action or its feet.
        // Still show this seat's newest authority instead of reintroducing the
        // remote interpolation delay merely because one input cannot be replayed.
        authoritative();
        return false;
    }
    turbo = restoreTurbo(state, seat.stats, seat.character, animator);
    replayFire = shieldLocomotion && (state.modifiers->shields & powerup::kFireShield) != 0;
    if (replayFire && (seat.fireShieldTree == nullptr ||
                       !fireShield.restore(*seat.fireShieldTree, state.companions[1]))) {
        authoritative();
        return false;
    }
    fireSeconds = replayFire ? state.modifiers->fireSeconds : 0;

    if (envelope.platform) {
        if (m_layout == nullptr || !m_geometry || !m_authority->geometry ||
            !m_authority->worldMotion || !state.floor || !state.rollback ||
            commands.size() > m_authority->worldMotionTicks ||
            (state.floor->flags & FloorRiding::kKeptApart) != 0 ||
            std::ranges::any_of(m_authority->players, [](const auto& other) {
                return other && other->life == ReplicaPlayerLife::Standing && other->floor &&
                       (other->floor->flags & FloorRiding::kKeptApart) != 0;
            })) {
            authoritative();
            return false;
        }
        worldScene = *m_geometry;
        worldCollision = m_collision;
        worldMotion.bind(*m_layout);
        if (!worldMotion.prepare(*m_authority->worldMotion, *m_authority->geometry, *worldScene,
                                 *worldCollision)) {
            authoritative();
            return false;
        }
        rider.floor = {state.floor->object, state.floor->flags, state.floor->placement};
        if (!retainSupport(rider.floor.object) || certifiedAncestor < 0) {
            authoritative();
            return false;
        }
    }
    contacts = state.locomotion ? &*state.locomotion : nullptr;
    stationaryAction = contacts && commands.size() > contacts->ticks;
    if (stationaryAction &&
        (onlyLocomotion || potionReplay || !state.rollback || !state.floor || !envelope.ranged ||
         contacts->automatic || !PlayerAnimator::rollbackAction(animator.action()) ||
         std::ranges::any_of(commands, [](const auto& command) {
             return SessionInputs::playInput(command.input).move.any();
         }))) {
        authoritative();
        return false;
    }
    // The native floor owner is part of movement, even without a moving carrier.
    // A complete contact roster permits following static slopes and stair treads;
    // legacy drawing-only checkpoints without that roster remain flat-ground only.
    staticTerrain = contacts && state.floor && !worldScene;

    if (staticTerrain) {
        if (state.floor->object < 0 || state.floor->placement ||
            (state.floor->flags & (FloorRiding::kMoving | FloorRiding::kKeptApart)) != 0 ||
            collision().moving(state.floor->object) ||
            collision().floorExitBlocked(state.floor->object) || !staticGrounded()) {
            authoritative();
            return false;
        }
        rider.floor = {state.floor->object, state.floor->flags, state.floor->placement};
    }
    if (contacts) {
        for (const auto& body : contacts->bodies) {
            bodies.push_back(body.target());
        }
        contactParty.resize(contacts->party.size() + 1);
        for (usize i = 0; i < contacts->party.size(); ++i) {
            const auto& body = contacts->party[i];
            contactParty[i + 1].actor.restoreMotion(body.base, 0, 0, body.radius, body.height, 0);
            contactParty[i + 1].life = PlayerLife::Standing;
        }
    }
    if (stationaryAction && !stationaryClear()) {
        authoritative();
        return false;
    }

    replay.endTick = m_authority->motion.tick;
    return true;
}
bool LocalMovementPrediction::Continuation::stationaryClear() {
    // A delayed acknowledgement can outlive the short dynamic-contact
    // certificate even when every retained input leaves the feet still.
    // Permit only local action/facing presentation in that case: establish
    // an untouched baseline, then require zero translation at every native
    // movement phase. Do not extend frozen body responses or speculate any
    // world/party impulse. New authority can still cancel the local action.
    const auto touchingBody = [&](const auto& body) {
        const Vec3 offset = actor.position() - body.base;
        return actor.position().y < body.base.y + body.height &&
               actor.position().y + actor.height() > body.base.y &&
               std::hypot(offset.x, offset.z) <= actor.radius() + body.radius;
    };
    const auto touchingGenerator = [&](const auto& box) {
        return box.solid && actor.position().y < box.centre.y + box.height &&
               actor.position().y + actor.height() > box.centre.y &&
               box.touchedBy({actor.position().x, box.centre.y, actor.position().z}, actor.radius(),
                             0);
    };
    contactParty.front().actor = actor;
    Vec3 position = actor.position();
    if (std::ranges::any_of(contacts->bodies, touchingBody) ||
        std::ranges::any_of(contacts->generators, touchingGenerator) ||
        std::ranges::any_of(contacts->fixtures,
                            [&](const auto& box) {
                                return box.solid &&
                                       box.touchedBy(actor.position(), actor.radius(), 0);
                            }) ||
        BodyCollision::resolveMovement(actor, position, position, bodies, contacts->generators,
                                       &collision()) != position ||
        PartyCollision::resolve(contactParty, 0, actor.position(), position)) {
        // Even a coincident/tangent party contact with unchanged feet calls
        // Knockback::shove in native play. It is not an inert zero-length step.
        return false;
    }
    return true;
}
bool LocalMovementPrediction::Continuation::replaceContacts(const DynamicContacts& dynamic) {
    using Identity = PlayerLocomotionState::Identity;
    if (lane != Lane::Ordinary || !state.locomotion || state.locomotion->automatic || worldScene ||
        dynamic.swarm.size() > PlayerLocomotionState::kMaxBodies ||
        dynamic.generators.size() > PlayerLocomotionState::kMaxGenerators) {
        return false;
    }
    const auto input = SessionInputs::playInput(commands[next].input);
    if (animator.meleeing() || (!envelope.ranged && (input.attack || input.strongAttack))) {
        // Contact geometry alone cannot refresh a melee acquisition/aim query.
        return false;
    }
    PlayerLocomotionState replacement = *state.locomotion;
    replacement.bodies.clear();
    replacement.generators.clear();
    replacement.generatorIdentities.clear();
    std::optional<u32> previous;
    for (const auto& body : dynamic.swarm) {
        if (body.identity.kind != Identity::Kind::Swarm ||
            (previous && body.identity.slot <= *previous)) {
            return false;
        }
        if (std::ranges::any_of(state.locomotion->bodies, [&](const auto& captured) {
                return captured.identity.kind == Identity::Kind::Swarm &&
                       captured.identity.slot == body.identity.slot &&
                       body.identity.instance < captured.identity.instance;
            })) {
            return false;
        }
        previous = body.identity.slot;
        replacement.bodies.push_back(body);
    }
    for (const auto& body : state.locomotion->bodies) {
        if (body.identity.kind != Identity::Kind::Swarm) {
            replacement.bodies.push_back(body);
        }
    }
    previous.reset();
    for (const auto& generator : dynamic.generators) {
        if (generator.identity.kind != Identity::Kind::Generator ||
            (previous && generator.identity.slot <= *previous)) {
            return false;
        }
        previous = generator.identity.slot;
        replacement.generators.push_back(generator.box);
        replacement.generatorIdentities.push_back(generator.identity);
    }
    for (usize contact = 0; contact < state.locomotion->generators.size(); ++contact) {
        const auto identity = state.locomotion->generatorIdentities.empty()
                                  ? Identity{}
                                  : state.locomotion->generatorIdentities[contact];
        if (identity.kind != Identity::Kind::Generator) {
            replacement.generators.push_back(state.locomotion->generators[contact]);
            replacement.generatorIdentities.push_back(identity);
        }
    }
    if (!replacement.valid()) {
        return false;
    }
    contactOverlay = std::move(replacement);
    contacts = &*contactOverlay;
    bodies.clear();
    for (const auto& body : contacts->bodies) {
        bodies.push_back(body.target());
    }
    // The long-latency stationary visual lane still never applies dynamic
    // responses. Fresh contacts must leave its unchanged feet untouched too.
    return !stationaryAction || stationaryClear();
}
void LocalMovementPrediction::Continuation::resetContacts() {
    if (!contactOverlay) {
        return;
    }
    contactOverlay.reset();
    contacts = &*state.locomotion;
    bodies.clear();
    for (const auto& body : contacts->bodies) {
        bodies.push_back(body.target());
    }
}
bool LocalMovementPrediction::Continuation::retainSupport(s32 object) {
    if (!worldScene) {
        return true;
    }
    const auto& objects = m_layout->objects();
    std::vector<u32> ancestry;
    for (usize guard = 0;
         object >= 0 && static_cast<usize>(object) < objects.size() && guard < objects.size();
         ++guard) {
        const auto objectIndex = static_cast<usize>(object);
        if (worldScene->moving(objectIndex)) {
            ancestry.push_back(static_cast<u32>(objectIndex));
        }
        object = objects[objectIndex].parent;
    }
    if (ancestry.empty()) {
        return true;
    }
    const auto highest = static_cast<s32>(ancestry.back());
    if (certifiedAncestor >= 0 && highest != certifiedAncestor) {
        // Host isolation covers the original carrier subtree, not occupants
        // on a second moving structure reached by future local footsteps.
        return false;
    }
    certifiedAncestor = highest;
    carriedObjects.insert(carriedObjects.end(), ancestry.begin(), ancestry.end());
    return true;
}
bool LocalMovementPrediction::Continuation::staticGrounded() const {
    const auto floor = FloorRiding::support(actor, collision());
    return floor && floor->object >= 0 &&
           (floor->objectFlags & (FloorRiding::kMoving | FloorRiding::kKeptApart)) == 0 &&
           !collision().moving(floor->object) && !collision().floorExitBlocked(floor->object) &&
           std::abs(floor->y - actor.position().y) <= 0.0001f;
}
bool LocalMovementPrediction::Continuation::resolveContacts(const Vec3& before) {
    if (stationaryAction) {
        return actor.position() == motion.position;
    }
    if (!contacts) {
        return true;
    }
    actor.slide(BodyCollision::resolveMovement(actor, before, actor.position(), bodies,
                                               contacts->generators, &collision()) -
                    actor.position(),
                &collision());
    contactParty.front().actor = actor;
    Vec3 position = actor.position();
    // Party contact also shoves the other actor. Until both impulses are in
    // this replay, never predict just one side of that shared interaction.
    return !PartyCollision::resolve(contactParty, 0, before, position);
}
bool LocalMovementPrediction::Continuation::stepOrdinary(
    const RollbackInputHistory::Entry& command) {
    // These are one-step contacts, not persistent actor state. PartyMotion
    // clears them before carry/impulses on every native update too.
    actor.clearWallContacts();
    if (worldScene && !worldMotion.beforeMotion(seconds, *worldScene, *worldCollision)) {
        authoritative();
        return false;
    }
    if (replayFire) {
        // The scene advances inventory and chooses the companion before
        // PartyMotion changes the action. Entering/leaving ShieldRun therefore
        // changes the blaze one step later than the body's new stance.
        if (fireSeconds >= 0) {
            fireSeconds = std::max(0.0f, fireSeconds - state.modifiers->inventoryStep);
        }
        const f32 alpha = fireSeconds < 0 ? 1 : std::clamp(fireSeconds, 0.0f, 1.0f);
        if (!fireShield.step(animator.action() == PlayerAnimator::Action::ShieldRun, seconds,
                             alpha)) {
            authoritative();
            return false;
        }
    }
    const auto input = SessionInputs::playInput(command.input);
    auto move = input.move;
    if (input.aimDirection) {
        const auto facing =
            actor.position() +
            (glm::length(*input.aimDirection) > 0.01f ? *input.aimDirection : actor.facing());
        move = cursorRelativeMove(move, actor.position(), facing, envelope.cameraYaw);
        if (animator.turnScale() >= 1.0f) {
            actor.faceToward(facing);
        }
    }
    const bool attackHeld = input.attack || input.strongAttack;
    const bool meleeSelection = animator.meleeing() || (attackHeld && !envelope.ranged);
    const Vec3 attackFacing = actor.facing();
    if (meleeSelection) {
        // Acquisition distances and visibility belong to these exact host
        // feet. Walking first, falling, or starting a stepping chain needs
        // world/contact replay, not a frozen-foot approximation.
        if ((weaponFlags(state) & 0xFU) != 0 || !state.melee || move.any() ||
            glm::distance(actor.position(), state.melee->selection.feet) > 0.0001f) {
            authoritative();
            return false;
        }
        if (!input.aimDirection) {
            const auto matches = [&](f32 yaw) {
                return std::abs(std::remainder(actor.yaw() - yaw, kTwoPi)) <= 0.0001f;
            };
            bool knownFacing = matches(state.melee->captureYaw);
            if (state.melee->assistedAim) {
                const Vec3 offset = *state.melee->assistedAim - actor.position();
                if (std::hypot(offset.x, offset.z) > 0.00001f) {
                    knownFacing = knownFacing || matches(std::atan2(offset.x, offset.z));
                }
            }
            if (!knownFacing) {
                authoritative();
                return false;
            }
        }
    }
    PlayerDeed deed = PlayerDeed::None;
    if (potion && potion->hasPotion() && input.usePotion) {
        deed = PlayerDeed::UsePotion;
    } else if (potion && potion->hasPotion() && input.throwPotion) {
        deed = PlayerDeed::ThrowPotion;
    } else if (input.defendPressed) {
        deed = PlayerDeed::Defend;
    } else if (input.strongAttack) {
        deed = PlayerDeed::StrongAttack;
    } else if (input.attack) {
        deed = PlayerDeed::Attack;
    }
    if (meleeSelection && !input.defendPressed && attackHeld) {
        const auto sense =
            MeleeSelection::sense(state.melee->selection, true, attackFacing, actor.yaw());
        deed = MeleeSelection::deed(sense, input.strongAttack, false, animator.meleeChain());
    }
    const bool autoForward =
        !input.aimDirection || (input.move.direction.x == 0 && input.move.direction.y > 0);
    if (contacts && contacts->automatic && deed == PlayerDeed::None && move.any() &&
        !input.strafe && autoForward && !input.attackPressed && !input.chargePressed &&
        !input.turboAttackPressed && animator.canAutoMelee()) {
        if (!contacts->automaticGeometry) {
            authoritative();
            return false;
        }
        const auto& automaticGeometry = *contacts->automaticGeometry;
        const auto query = MeleeGeometry::snapshot(
            automaticGeometry.moving->targets, actor.position(), actor.height(), actor.reach(),
            automaticGeometry.selection.range, automaticGeometry.selection.facingDot,
            actor.followPoint(), actor.followPoint(), actor.followPoint(), &collision());
        if (MeleeSelection::sense(query, false, attackFacing, actor.yaw()).range ==
            MeleeRange::Swing) {
            authoritative();
            return false;
        }
    }
    const f32 pace = PartyMotion::actionPace(&animator, deed);
    const bool strafe = (input.strafe || input.aimDirection) && move.any();
    const bool cursorForward = input.aimDirection && !input.strafe && !input.attack &&
                               input.move.direction.x == 0 && input.move.direction.y > 0;
    animator.setStrafe(strafe && !cursorForward
                           ? PartyMotion::strafeWayOf(
                                 PlayerActor::headingOf(move, envelope.cameraYaw), actor.yaw())
                           : StrafeWay::None);
    if (worldScene) {
        FloorRiding::carry(rider, collision());
    }
    const auto before = actor.position();
    if (state.knockback) {
        idleImpulse.step(seconds, actor.speed());
    }
    const auto attackMove =
        PartyMotion::attackMove(actor, &animator, move, deed, envelope.cameraYaw, seconds, false);
    actor.update(attackMove, envelope.cameraYaw, seconds, &collision(), pace, strafe);
    if (!resolveContacts(before)) {
        authoritative();
        return false;
    }
    if (worldScene || staticTerrain) {
        FloorRiding::land(platformParty, 0, before, collision());
        if (!retainSupport(rider.floor.object) ||
            (stationaryAction && actor.position() != motion.position)) {
            authoritative();
            return false;
        }
    }
    if (envelope.cameraLimited) {
        const auto constrained = CameraMovementLimit::constrain(
            before, actor.position(), envelope.attention, m_authority->motion.camera,
            {m_authority->motion.horizontalFov, m_authority->motion.aspect},
            Vec3{0, envelope.followHeight, 0});
        if (glm::distance(constrained, actor.position()) > 1.0e-5f) {
            actor.place(before);
            actor.slide(constrained - before, &collision());
            if (!resolveContacts(before)) {
                authoritative();
                return false;
            }
            if (worldScene || staticTerrain) {
                FloorRiding::land(platformParty, 0, before, collision());
                if (!retainSupport(rider.floor.object)) {
                    authoritative();
                    return false;
                }
            }
        }
    }
    actor.fall(seconds, collision());
    if ((staticTerrain && !staticGrounded()) ||
        (stationaryAction && actor.position() != motion.position)) {
        // Rescue spots, freefall, kept-apart floors and moving supports depend
        // on more than the immutable contact roster. Do not guess those paths.
        authoritative();
        return false;
    }
    // Crossing onto a moving support requires the host's rider state.
    if (const auto floor = collision().floorAt(actor.position(), 0.25f, 0.25f);
        floor && collision().moving(floor->object) && !worldScene) {
        fallback = false;
        return false;
    }
    // PartyMotion turns a stationary ordinary throw toward its assisted target
    // after moving the feet, before advancing the action. Explicit cursor aim,
    // strafe and requested movement retain their own heading. Weapon ballistics
    // use the follow point separately; body facing is measured from the feet.
    const bool ordinaryAttack = deed == PlayerDeed::Attack || deed == PlayerDeed::StrongAttack ||
                                deed == PlayerDeed::Melee || deed == PlayerDeed::MeleeLow ||
                                deed == PlayerDeed::MeleeSlow || deed == PlayerDeed::MeleeSlowLow;
    if (!input.aimDirection && !move.any() && !input.strafe && ordinaryAttack) {
        std::optional<Vec3> assisted;
        if (meleeSelection) {
            assisted = state.melee->assistedAim;
        } else if (state.weapon) {
            assisted = state.weapon->target;
        }
        if (assisted) {
            actor.faceToward(*assisted);
        }
    }
    if (meleeSelection) {
        if (glm::distance(actor.position(), state.melee->selection.feet) > 0.0001f) {
            authoritative();
            return false;
        }
        // PartyMotion retains the pre-assist acquisition facing, but its
        // refinement yaw is measured against the newly turned body. Re-query
        // without held reach after buttons are released during recovery.
        animator.setMelee(
            MeleeSelection::sense(state.melee->selection, attackHeld, attackFacing, actor.yaw()));
    }
    // Native update preserves 30 Hz shot boundaries internally while accepting
    // interrupts every application tick. Observe fresh releases for speculative
    // presentation, without dispatching damage, sound or any other side effect.
    // Current item flags can differ from the preceding frame's cached attack
    // speed after a pickup/expiry. Only native sequence changes relatch speed.
    animator.setAttackSpeed(rapidFire(state), speedBoost(state));
    // PartyMotion samples armor here, after applying this step's old action
    // pace. Restoring a checkpoint must not relatch the shield stance early.
    animator.setShielded(shieldLocomotion);
    if (potion) {
        if (!potion->step({command.input.tick, PlayerAnimator::motionFor(move.magnitude),
                           animator.checkpoint().strafe, deed, actor.position(), actor.facing()})) {
            authoritative();
            return false;
        }
        animator = potion->animator();
    } else {
        animator.update(PlayerAnimator::motionFor(move.magnitude), 1, seconds, deed);
    }
    if (turbo) {
        turbo->advance(animator, actor.facing(), seconds);
    }
    if (state.knockback) {
        idleImpulse.endFrame();
    }
    if ((onlyLocomotion && !PlayerAnimator::locomotion(animator.action())) ||
        (!PlayerAnimator::rollbackAction(animator.action()) &&
         !(meleeSelection && PlayerAnimator::rollbackMeleeAction(animator.action())) &&
         !(potion && PlayerPotionState::supports(animator.checkpoint())))) {
        // Even a stationary quick chain can request Step2/3 when its target
        // is now a step away. Never animate that lunge with immobile feet.
        authoritative();
        return false;
    }
    if (worldScene && !worldMotion.afterMotion(seconds, *worldScene, *worldCollision)) {
        authoritative();
        return false;
    }
    replay.endTick = command.input.tick;
    recordFrame(replay, command.input.tick, motion, actor, animator, turbo);
    const auto release = [&](bool strong) {
        auto releasedMotion = motion;
        releasedMotion.position = actor.position();
        releasedMotion.yaw = std::remainder(actor.yaw(), kTwoPi);
        replay.releases.push_back({command.input.tick, animator.player().generation(),
                                   releasedMotion, strong, input.aimDirection.has_value(),
                                   animator.attackSeconds(), weaponShots(state),
                                   weaponFlags(state)});
        auto& observed = replay.releases.back();
        if (input.aimDirection) {
            observed.targetResolved = true;
        } else if (!envelope.ranged && state.melee &&
                   glm::distance(actor.position(), state.melee->selection.feet) <= 0.0001f) {
            const auto matches = [&](f32 yaw) {
                return std::abs(std::remainder(actor.yaw() - yaw, kTwoPi)) <= 0.0001f;
            };
            bool knownFacing = matches(state.melee->captureYaw);
            if (state.melee->assistedAim) {
                const Vec3 offset = *state.melee->assistedAim - actor.position();
                if (std::hypot(offset.x, offset.z) > 0.00001f) {
                    knownFacing = knownFacing || matches(std::atan2(offset.x, offset.z));
                }
            }
            if (knownFacing) {
                // Stationary capture verifies that native assisted turning is
                // a fixed point. This remains valid only at those same feet
                // and one of the two certified bearings.
                observed.targetResolved = true;
                observed.target = state.melee->assistedAim;
            }
        }
    };
    if (animator.released()) {
        release(false);
    }
    if (animator.strongReleased()) {
        release(true);
    }
    // Fixtures resolve after PartyMotion emits its attacks. Moving these
    // pushes before the release changes the native launch point and aim.
    if (contacts && !stationaryAction) {
        for (const auto& box : contacts->fixtures) {
            actor.slide(box.pushOut(actor.position(), actor.radius()) - actor.position(),
                        &collision());
        }
        const Vec3 offset = actor.position() - contacts->origin;
        // The collision roster is captured horizontally at all elevations.
        // Automatic melee's separate acquisition certificate is not: retain
        // its original height until that query has a vertical motion bound.
        const bool heightUnsupported =
            (!staticTerrain || contacts->automatic) && std::abs(offset.y) > 0.0001f;
        if (std::hypot(offset.x, offset.z) > contacts->radius || heightUnsupported ||
            (staticTerrain && !staticGrounded())) {
            authoritative();
            return false;
        }
    }
    return true;
}
void LocalMovementPrediction::Continuation::finishOrdinary() {
    seat.pose = motion;
    seat.pose->position = actor.position();
    seat.pose->yaw = std::remainder(actor.yaw(), kTwoPi);
    const auto& cursor = animator.player();
    seat.animation = {static_cast<u32>(animator.action()), cursor.sequence(), cursor.generation(),
                      cursor.presentationFrame(), cursor.transition()};
    if (state.rollback) {
        if (potion) {
            const auto releases = potion->releases();
            replay.potions.assign(releases.begin(), releases.end());
        }
        seat.checkpoint = animator.checkpoint();
        seat.transitionPose = animator.transitionPose();
        seat.replay = std::move(replay);
    }
    if (state.knockback) {
        seat.knockback = idleImpulse.checkpoint();
    }
    if (turbo) {
        seat.turbo = turbo->checkpoint();
    }
    if (replayFire) {
        seat.companionReplayed = true;
        seat.fireShield = fireShield.sample(glm::scale(actor.transform(), Vec3{seat.bodyScale}));
    }
    if (worldScene || staticTerrain) {
        seat.floor = PlayerFloorState{rider.floor.object, rider.floor.flags, rider.floor.placement};
    }
    if (worldScene) {
        seat.geometry = worldScene->geometry();
        std::ranges::sort(carriedObjects);
        carriedObjects.erase(std::unique(carriedObjects.begin(), carriedObjects.end()),
                             carriedObjects.end());
        seat.geometryObjects = std::move(carriedObjects);
        if (state.shadow) {
            seat.shadow = state.shadow;
            seat.shadow->ground += actor.position() - motion.position;
            if (const auto floor = collision().floorAt(actor.position(), 0.25f, 3)) {
                seat.shadow->ground.y = floor->y;
                seat.shadow->normal = floor->normal;
            }
        }
        seat.correction = seat.previousCorrection = Vec3{0};
    }
}
bool LocalMovementPrediction::Continuation::prepareBounded() {
    if ((weaponFlags(state) & 0xFU) != 0 || !state.rollback || !geometry->moving ||
        (!PlayerAnimator::rollbackAction(animator.action()) &&
         !PlayerAnimator::rollbackMovingMeleeAction(animator.action()) &&
         !(turboReplay && (PlayerAnimator::rollbackTurboAction(animator.action()) ||
                           PlayerAnimator::rollbackShoveAction(animator.action()))))) {
        return false;
    }
    if (commands.size() > geometry->moving->ticks ||
        std::ranges::any_of(commands, [this](const auto& command) {
            return !supported(command.input, state, turboReplay);
        })) {
        return false;
    }
    turbo = restoreTurbo(state, seat.stats, seat.character, animator);
    if (turboReplay && !turbo) {
        return false;
    }
    if (state.knockback && !impulse.restore(*state.knockback)) {
        return false;
    }
    for (const auto& target : geometry->moving->targets) {
        if (target.blocksMovement) {
            bodies.push_back(MeleeGeometry::body(target));
        }
    }

    if (!boundedGrounded()) {
        return false;
    }
    replay.endTick = m_authority->motion.tick;
    return true;
}
bool LocalMovementPrediction::Continuation::boundedGrounded() const {
    const auto floor = FloorRiding::support(actor, m_collision);
    return glm::distance(actor.position(), geometry->moving->origin) <= geometry->moving->radius &&
           floor && (floor->objectFlags & FloorRiding::kMoving) == 0 &&
           !m_collision.moving(floor->object) && !m_collision.floorExitBlocked(floor->object) &&
           std::abs(floor->y - actor.position().y) <= 0.0001f &&
           std::ranges::none_of(actor.wallContacts(), [&](const auto& contact) {
               return m_collision.moving(contact.object);
           });
}
bool LocalMovementPrediction::Continuation::fixturesClear() const {
    const Vec3 position = actor.position();
    return std::ranges::none_of(geometry->moving->obstacles, [&](const auto& obstacle) {
        // Fixtures additionally perform endpoint push-out after party motion.
        // Its authored vertical extent differs from the swept player-height
        // check, so neither query replaces the other.
        return glm::distance(obstacle.pushOut(position, actor.radius()), position) > 0.00001f;
    });
}
bool LocalMovementPrediction::Continuation::clearStep(const Vec3& before) const {
    const Vec3 after = actor.position();
    const auto body = BodyCollision::resolve(before, after, actor.radius(), actor.height(), bodies);
    const auto item = BodyCollision::resolveItems(before, body, actor.radius(), actor.height(),
                                                  geometry->moving->obstacles);
    // Contact can change both parties' impulses, targeting and future motion.
    // This lane only replays steps requiring no such correction. Apply the
    // native static-wall overlap check too, rather than omitting that callback.
    const auto wall =
        m_collision.resolveWalls(item, actor.radius(), item.y + PlayerActor::kFootClearance,
                                 item.y + actor.height() - PlayerActor::kFootClearance, nullptr,
                                 actor.minimumWallY(item.y), WorldCollision::WallPush::StaticOnly);
    return glm::distance(body, after) <= 0.00001f && glm::distance(item, after) <= 0.00001f &&
           glm::distance(wall, after) <= 0.00001f && fixturesClear() && boundedGrounded();
}
MeleeSelection::Snapshot LocalMovementPrediction::Continuation::acquisition(const Vec3& from,
                                                                            const Vec3& to) const {
    return MeleeGeometry::snapshot(geometry->moving->targets, actor.position(), actor.height(),
                                   actor.reach(), geometry->selection.range,
                                   geometry->selection.facingDot, actor.followPoint(), from, to,
                                   &m_collision);
}
bool LocalMovementPrediction::Continuation::stepBounded(
    const RollbackInputHistory::Entry& command) {
    actor.clearWallContacts();
    const auto input = SessionInputs::playInput(command.input);
    auto move = input.move;
    const bool steeringTurbo = PlayerAnimator::rollbackTurboAction(animator.action());
    const bool charging = animator.shoving();
    if (input.aimDirection) {
        const auto& toward = *input.aimDirection;
        const Vec3 facing =
            actor.position() + (std::hypot(toward.x, toward.z) < 0.01f ? actor.facing() : toward);
        move = cursorRelativeMove(move, actor.position(), facing, envelope.cameraYaw);
        if (steeringTurbo) {
            const auto offset = facing - actor.position();
            actor.turnTo(PartyMotion::turboHeading(actor.yaw(), std::atan2(offset.x, offset.z),
                                                   seconds, animator.turnScale()));
        } else if (!charging && animator.turnScale() >= 1.0f) {
            actor.faceToward(facing);
        }
    }
    Vec3 attackFacing = actor.facing();
    if (move.any() && !input.aimDirection && !input.strafe) {
        const f32 heading = PlayerActor::headingOf(move, envelope.cameraYaw);
        attackFacing = {std::sin(heading), 0, std::cos(heading)};
    }
    const bool attackHeld = input.attack || input.strongAttack;
    PlayerDeed deed = turboReplay ? PartyMotion::turboDeed(animator, turbo->meter.held(), input)
                                  : PlayerDeed::None;
    if (deed == PlayerDeed::None && input.defendPressed) {
        deed = PlayerDeed::Defend;
    } else if (deed == PlayerDeed::None && attackHeld) {
        const auto query = acquisition(actor.followPoint(), actor.followPoint());
        const auto sense = MeleeSelection::sense(query, true, attackFacing, actor.yaw());
        deed = MeleeSelection::deed(sense, input.strongAttack, move.any(), animator.meleeChain());
    }
    const f32 pace = PartyMotion::actionPace(&animator, deed);
    const bool strafe = !charging && (input.strafe || input.aimDirection) && move.any();
    const bool cursorForward = input.aimDirection && !input.strafe && !input.attack &&
                               input.move.direction.x == 0 && input.move.direction.y > 0;
    animator.setStrafe(strafe && !cursorForward
                           ? PartyMotion::strafeWayOf(
                                 PlayerActor::headingOf(move, envelope.cameraYaw), actor.yaw())
                           : StrafeWay::None);
    const Vec3 before = actor.position();
    const Vec3 attackFrom = actor.followPoint();
    if (state.knockback) {
        impulse.step(seconds, actor.speed());
    }
    const auto planned = PartyMotion::attackMove(actor, &animator, move, deed, envelope.cameraYaw,
                                                 seconds, true, steeringTurbo);
    actor.update(charging ? PartyMotion::chargeInput(actor, move, envelope.cameraYaw) : planned,
                 envelope.cameraYaw, seconds, &m_collision, pace, strafe);
    // Native contact sweeps stop before creature/party corrections. Only a
    // camera rewrite replaces this endpoint. Falling is later still.
    Vec3 attackTo = actor.followPoint();
    if (!clearStep(before)) {
        return false;
    }
    if (envelope.cameraLimited) {
        const auto limited = CameraMovementLimit::constrain(
            before, actor.position(), envelope.attention, m_authority->motion.camera,
            {m_authority->motion.horizontalFov, m_authority->motion.aspect},
            Vec3{0, envelope.followHeight, 0});
        if (glm::distance(limited, actor.position()) > 1.0e-5f) {
            actor.place(before);
            actor.slide(limited - before, &m_collision);
            attackTo = actor.followPoint();
            if (!clearStep(before)) {
                return false;
            }
        }
    }
    actor.fall(seconds, m_collision);
    // Falling can select a higher static floor than the earlier support
    // probe. Native fixture push-out runs afterward, at that final height.
    if (!boundedGrounded() || !fixturesClear()) {
        return false;
    }
    // Ram runs after motion, including the final Shove frame before the
    // animator completes it. Its contact margin exceeds body collision;
    // striking anything would change the replayed world's future geometry->
    if (charging && (std::ranges::any_of(geometry->moving->targets,
                                         [&](const auto& target) {
                                             return MeleeGeometry::body(target).touches(
                                                 actor.followPoint(),
                                                 actor.radius() + PlayerAttacks::kRamReach);
                                         }) ||
                     std::ranges::any_of(geometry->moving->obstacles, [&](const auto& obstacle) {
                         return obstacle.touchedBy(actor.position(), actor.radius(),
                                                   PlayerAttacks::kRamReach);
                     }))) {
        return false;
    }
    const auto query = acquisition(attackFrom, attackTo);
    const bool ordinaryAttack = deed == PlayerDeed::Attack || deed == PlayerDeed::StrongAttack ||
                                deed == PlayerDeed::Melee || deed == PlayerDeed::MeleeLow ||
                                deed == PlayerDeed::MeleeSlow || deed == PlayerDeed::MeleeSlowLow;
    if (!input.aimDirection && !move.any() && !charging && !input.strafe && ordinaryAttack) {
        if (const auto aim = MeleeGeometry::aim(
                geometry->moving->targets, actor.followPoint(), actor.facing(),
                geometry->selection.range, &m_collision, geometry->selection.facingDot, query)) {
            actor.faceToward(*aim);
        }
    }
    animator.setAttackSpeed(rapidFire(state), speedBoost(state));
    animator.setShielded(false);
    animator.setPushed(false);
    if (attackHeld || animator.meleeing()) {
        animator.setMelee(MeleeSelection::sense(query, attackHeld, attackFacing, actor.yaw()));
    }
    animator.setCombo(-1, false);
    animator.update(PlayerAnimator::motionFor(move.magnitude), 1, seconds, deed);
    if (turbo) {
        turbo->advance(animator, actor.facing(), seconds);
    }
    if (state.knockback) {
        impulse.endFrame();
    }
    if (!PlayerAnimator::rollbackAction(animator.action()) &&
        !PlayerAnimator::rollbackMovingMeleeAction(animator.action()) &&
        !(turboReplay && (PlayerAnimator::rollbackTurboAction(animator.action()) ||
                          PlayerAnimator::rollbackShoveAction(animator.action())))) {
        return false;
    }
    replay.endTick = command.input.tick;
    recordFrame(replay, command.input.tick, motion, actor, animator, turbo);
    if (turboReplay && turbo && state.weapon) {
        if (!turbo->volleys.empty() && !PlayerAnimator::rollbackTurboAction(animator.action())) {
            return false;
        }
        for (const auto& volley : turbo->volleys) {
            if (volley.emissionIdentity() == 0) {
                return false;
            }
            auto releasedMotion = motion;
            releasedMotion.position = actor.position();
            releasedMotion.yaw = std::remainder(actor.yaw(), kTwoPi);
            replay.releases.push_back({command.input.tick, animator.player().generation(),
                                       releasedMotion, false, input.aimDirection.has_value(),
                                       animator.attackSeconds(), 1, weaponFlags(state),
                                       volley.emissionIdentity(), volley.direction});
        }
    }
    const auto release = [&](bool strong) {
        auto releasedMotion = motion;
        releasedMotion.position = actor.position();
        releasedMotion.yaw = std::remainder(actor.yaw(), kTwoPi);
        replay.releases.push_back({command.input.tick, animator.player().generation(),
                                   releasedMotion, strong, input.aimDirection.has_value(),
                                   animator.attackSeconds(), weaponShots(state),
                                   weaponFlags(state)});
        auto& observed = replay.releases.back();
        observed.targetResolved = true;
        if (!input.aimDirection) {
            // Native release acquisition uses the final feet/body bearing and
            // this step's pre-correction contact sweep, not the snapshot's aim.
            observed.target = MeleeGeometry::aim(
                geometry->moving->targets, actor.followPoint(), actor.facing(),
                geometry->selection.range, &m_collision, geometry->selection.facingDot, query);
        }
    };
    if (animator.released()) {
        release(false);
    }
    if (animator.strongReleased()) {
        release(true);
    }
    return true;
}
void LocalMovementPrediction::Continuation::finishBounded() {
    seat.pose = motion;
    seat.pose->position = actor.position();
    seat.pose->yaw = std::remainder(actor.yaw(), kTwoPi);
    const auto& cursor = animator.player();
    seat.animation = {static_cast<u32>(animator.action()), cursor.sequence(), cursor.generation(),
                      cursor.presentationFrame(), cursor.transition()};
    seat.checkpoint = animator.checkpoint();
    seat.transitionPose = animator.transitionPose();
    if (turbo) {
        seat.turbo = turbo->checkpoint();
    }
    if (state.knockback) {
        seat.knockback = impulse.checkpoint();
    }
    seat.replay = std::move(replay);
}
bool LocalMovementPrediction::Continuation::prepareKnockback() {
    if (!m_authority || !state.rollback || !state.knockback || state.knockbackTicks == 0 ||
        state.knockbackTicks > PlayerCombatState::kKnockbackTicks ||
        commands.size() > state.knockbackTicks ||
        !PlayerAnimator::rollbackReactionDeed(state.reaction) ||
        (!PlayerAnimator::locomotion(animator.action()) &&
         !PlayerAnimator::rollbackReactionAction(animator.action()))) {
        return false;
    }
    const auto& checkpoint = *state.knockback;
    if ((checkpoint.flags & ~Knockback::kKnockBack) != 0 || checkpoint.shove != Vec3{0} ||
        checkpoint.shoved || checkpoint.pushed || !knockback.restore(checkpoint)) {
        return false;
    }

    if (!reactionGrounded()) {
        return false;
    }
    turbo = restoreTurbo(state, seat.stats, seat.character, animator);
    reaction = state.reaction;
    replay.endTick = m_authority->motion.tick;
    return true;
}
bool LocalMovementPrediction::Continuation::reactionGrounded() const {
    const auto floor = m_collision.floorAt(actor.position(), 0.25f, 0.25f);
    return floor && !m_collision.moving(floor->object) &&
           !m_collision.floorExitBlocked(floor->object) &&
           std::abs(floor->y - actor.position().y) <= 0.0001f &&
           std::abs(actor.position().y - motion.position.y) <= 0.0001f;
}
bool LocalMovementPrediction::Continuation::stepKnockback(
    const RollbackInputHistory::Entry& command) {
    const auto input = SessionInputs::playInput(command.input);
    // Inventory input remains live during native damage reactions. It can
    // enable items whose movement/animation state this lane cannot replay.
    if (input.selector.up || input.selector.down || input.selector.left || input.selector.right ||
        input.usePotion || input.throwPotion || input.shieldPotion) {
        return false;
    }
    const bool reeling = reaction != PlayerDeed::None || animator.reacting();
    if (!reeling &&
        (input.move.any() || input.aimDirection || input.attack || input.strongAttack ||
         input.defendPressed || input.usePotion || input.throwPotion || input.shieldPotion ||
         input.combo || input.chargePressed || input.turboAttackPressed)) {
        // The host certified only the deterministic no-input slide. Once the
        // body recovers, wait for new targeting/contact context rather than
        // starting an attack from pre-impact distances or walking past bodies.
        return false;
    }
    actor.clearWallContacts();
    animator.setStrafe(StrafeWay::None);
    const Vec3 before = actor.position();
    // PartyMotion applies the existing slide BEFORE turning this frame's
    // queued damage into a new kick. Swapping these gives an extra early step.
    actor.slide(knockback.step(seconds, actor.speed()), &m_collision);
    if (const auto heading = knockback.kick(actor.yaw(), false)) {
        actor.turnTo(*heading);
    }
    actor.update({}, state.movement.cameraYaw, seconds, &m_collision, animator.moveScale());
    if (state.movement.cameraLimited) {
        const auto constrained = CameraMovementLimit::constrain(
            before, actor.position(), state.movement.attention, m_authority->motion.camera,
            {m_authority->motion.horizontalFov, m_authority->motion.aspect},
            Vec3{0, state.movement.followHeight, 0});
        if (glm::distance(constrained, actor.position()) > 1.0e-5f) {
            actor.place(before);
            actor.slide(constrained - before, &m_collision);
        }
    }
    actor.fall(seconds, m_collision);
    if (!reactionGrounded() || std::ranges::any_of(actor.wallContacts(), [&](const auto& contact) {
            return m_collision.moving(contact.object);
        })) {
        return false;
    }
    // Host admission excludes unsupported items, combos and party shoves.
    // This native update never dispatches damage, audio, rumble or releases.
    animator.setAttackSpeed(rapidFire(state), speedBoost(state));
    animator.setShielded(false);
    animator.setPushed(false);
    animator.setCombo(-1, false);
    animator.update(PlayerMotion::Stand, 1, seconds, reaction);
    if (turbo) {
        turbo->advance(animator, actor.facing(), seconds);
    }
    reaction = PlayerDeed::None;
    knockback.endFrame();
    if (!PlayerAnimator::locomotion(animator.action()) &&
        !PlayerAnimator::rollbackReactionAction(animator.action())) {
        return false;
    }
    replay.endTick = command.input.tick;
    recordFrame(replay, command.input.tick, motion, actor, animator, turbo);
    return true;
}
void LocalMovementPrediction::Continuation::finishKnockback() {
    seat.pose = motion;
    seat.pose->position = actor.position();
    seat.pose->yaw = std::remainder(actor.yaw(), kTwoPi);
    const auto& cursor = animator.player();
    seat.animation = {static_cast<u32>(animator.action()), cursor.sequence(), cursor.generation(),
                      cursor.presentationFrame(), cursor.transition()};
    seat.checkpoint = animator.checkpoint();
    seat.transitionPose = animator.transitionPose();
    seat.knockback = knockback.checkpoint();
    if (turbo) {
        seat.turbo = turbo->checkpoint();
    }
    seat.reaction = reaction;
    seat.replay = std::move(replay);
}
bool LocalMovementPrediction::Continuation::step() {
    if (!pending()) {
        return false;
    }
    const auto& command = commands[next];
    bool advanced = false;
    switch (lane) {
    case Lane::Ordinary: advanced = stepOrdinary(command); break;
    case Lane::Bounded: advanced = stepBounded(command); break;
    case Lane::Reaction: advanced = stepKnockback(command); break;
    }
    if (!advanced) {
        failed = true;
        if (fallback) {
            authoritative();
        }
        return false;
    }
    ++next;
    return true;
}
void LocalMovementPrediction::Continuation::finish() {
    if (failed) {
        return;
    }
    switch (lane) {
    case Lane::Ordinary: finishOrdinary(); break;
    case Lane::Bounded: finishBounded(); break;
    case Lane::Reaction: finishKnockback(); break;
    }
}
LocalMovementPrediction::ReplayActor LocalMovementPrediction::Continuation::observation() const {
    ReplayActor result;
    result.status = ReplayStatus::Complete;
    if (failed) {
        result.status = ReplayStatus::Unsupported;
    } else if (pending()) {
        result.status = ReplayStatus::Active;
    }
    result.tick = failed ? m_authority->motion.tick : replay.endTick;
    result.motion = motion;
    if (failed) {
        result.animation = state.rollback;
    } else {
        result.motion->position = actor.position();
        result.motion->yaw = std::remainder(actor.yaw(), kTwoPi);
        result.animation = animator.checkpoint();
    }
    return result;
}
void LocalMovementPrediction::present(CombatSnapshot& displayed, f32 blend) const {
    if (!m_authority || displayed.motion.epoch != m_context.epoch) {
        return;
    }
    blend = std::clamp(blend, 0.0f, 1.0f);
    for (usize i = 0; i < m_seats.size(); ++i) {
        const auto& seat = m_seats[i];
        const auto& displayedMotion = displayed.motion.players[i];
        if (!seat.pose || !displayed.players[i] || !displayedMotion ||
            displayedMotion->grant != seat.pose->grant ||
            displayedMotion->continuity != seat.pose->continuity ||
            (seat.geometry &&
             (!displayed.geometry || displayed.geometry->layout != seat.geometry->layout))) {
            continue;
        }
        const auto movingFloor = [](const PlayerCombatState& player) {
            return player.movement.platform ||
                   (player.floor &&
                    (player.floor->placement || (player.floor->flags & FloorRiding::kMoving) != 0));
        };
        if (seat.authoritativeOnly &&
            (movingFloor(*m_authority->players[i]) || movingFloor(*displayed.players[i]))) {
            // A refused/expired replay has no certified geometry endpoint. Keep
            // both the rider and its floor on the delayed presentation clock;
            // copying newer feet alone would hover over or sink into that floor.
            // Other actors/items on the carrier must not be advanced to fix it.
            continue;
        }
        std::optional<SceneGeometry> geometry;
        if (seat.geometry) {
            geometry = displayed.geometry;
            // An older presentation checkpoint may omit an override which was
            // introduced by the latest support state. Merge the sparse state,
            // including omission's authored-default meaning, before moving feet.
            for (const u32 object : seat.geometryObjects) {
                const auto predicted = std::ranges::lower_bound(seat.geometry->objects, object, {},
                                                                &SceneGeometry::Object::index);
                const auto shown = std::ranges::lower_bound(geometry->objects, object, {},
                                                            &SceneGeometry::Object::index);
                const bool hasPredicted =
                    predicted != seat.geometry->objects.end() && predicted->index == object;
                const bool hasShown = shown != geometry->objects.end() && shown->index == object;
                if (hasPredicted && hasShown) {
                    *shown = *predicted;
                } else if (hasPredicted) {
                    geometry->objects.insert(shown, *predicted);
                } else if (hasShown) {
                    geometry->objects.erase(shown);
                }
            }
            if (!geometry->valid()) {
                continue;
            }
        }
        auto pose = *seat.pose;
        pose.position += seat.correction;
        const auto& source = *m_authority->motion.players[i];
        auto state = *m_authority->players[i];
        if (!seat.authoritativeOnly && !seat.geometry && seat.previous &&
            seat.previous->continuity == pose.continuity &&
            glm::distance(seat.previous->position, pose.position) < 0.75f) {
            pose.position =
                glm::mix(seat.previous->position + seat.previousCorrection, pose.position, blend);
            pose.yaw = std::remainder(
                seat.previous->yaw + std::remainder(pose.yaw - seat.previous->yaw, kTwoPi) * blend,
                kTwoPi);
            // A smoothed correction is still not allowed to cut a corner through
            // collision or interpolate underneath the tread of a stair.
            PlayerActor contact;
            const auto& movement = state.movement;
            contact.restoreMotion(seat.pose->position, pose.yaw, movement.speed, movement.radius,
                                  movement.height, movement.followHeight);
            contact.slide(pose.position - seat.pose->position, &m_collision);
            pose.position = contact.position();
        }
        const auto offset = pose.position - source.position;
        state.animation = seat.animation;
        state.rollback = seat.checkpoint;
        state.transitionPose = seat.transitionPose;
        state.locomotion.reset();
        state.potion.reset();
        state.knockback = seat.knockback;
        // Every replacement here is mixed-age drawing data, not a renewed host
        // world-motion permission, including newest-authority action fallbacks.
        state.movement.platform = false;
        if (seat.floor) {
            state.floor = seat.floor;
        }
        if (seat.turbo) {
            state.turbo = seat.turbo;
            state.weaponHidden = seat.turbo->move.weaponHidden;
        }
        state.reaction = seat.reaction;
        // This mixed-age drawing snapshot is not a new authoritative baseline.
        // Its world tick can predate the last input of this locally predicted seat.
        state.rollbackInput.reset();
        if (!seat.authoritativeOnly && !seat.geometry && seat.previous &&
            seat.previousAnimation.action == seat.animation.action &&
            seat.previousAnimation.sequence == seat.animation.sequence &&
            seat.previousAnimation.generation == seat.animation.generation &&
            seat.previousAnimation.frame <= seat.animation.frame) {
            state.animation.frame =
                std::lerp(seat.previousAnimation.frame, seat.animation.frame, blend);
        }
        if (seat.geometry) {
            state.shadow = seat.shadow;
        } else if (state.shadow) {
            state.shadow->ground += offset;
            if (const auto floor = m_collision.floorAt(pose.position, 0.25f, 3)) {
                state.shadow->ground.y = floor->y;
                state.shadow->normal = floor->normal;
            }
        }
        const auto pivot = glm::translate(Mat4{1}, pose.position) *
                           glm::rotate(Mat4{1}, pose.yaw - source.yaw, Vec3{0, 1, 0}) *
                           glm::translate(Mat4{1}, -source.position);
        for (auto& companion : state.companions) {
            if (companion) {
                companion->placement = pivot * companion->placement;
            }
        }
        if (seat.companionReplayed) {
            state.companions[1] = seat.fireShield;
            if (state.companions[1]) {
                // The rebuilt companion already belongs to the replay endpoint,
                // unlike the other companions borrowed from the host checkpoint.
                const auto localPivot =
                    glm::translate(Mat4{1}, pose.position) *
                    glm::rotate(Mat4{1}, pose.yaw - seat.pose->yaw, Vec3{0, 1, 0}) *
                    glm::translate(Mat4{1}, -seat.pose->position);
                state.companions[1]->placement = localPivot * state.companions[1]->placement;
            }
        }
        displayed.motion.players[i] = pose;
        displayed.players[i] = state;
        if (seat.geometry) {
            // Only the rider's support ancestry belongs to the local clock.
            // Advancing unrelated platforms would separate remote actors/items
            // from the delayed floors which still own their presentation.
            displayed.geometry = std::move(geometry);
        }
    }
}
} // namespace gdl::game
