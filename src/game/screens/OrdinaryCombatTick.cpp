#include "game/screens/OrdinaryCombatTick.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <iterator>
#include <utility>

#include "game/players/PowerupEffects.h"
#include "game/players/Progression.h"
#include "game/screens/EnemyFeedbackEmission.h"
#include "game/screens/HelpMessages.h"
#include "game/screens/OrdinaryBarrelEmission.h"
#include "game/screens/OrdinaryCritterPhase.h"
#include "game/screens/OrdinaryFixturePhase.h"
#include "game/screens/OrdinaryFloorSupport.h"
#include "game/screens/OrdinaryProjectilePhase.h"
#include "game/screens/PartyFigures.h"
#include "game/screens/PartyHelpAdmission.h"
#include "game/screens/PartyNames.h"
#include "game/screens/PartyRecords.h"
#include "game/screens/PlayerPowerups.h"
#include "game/world/CameraMovementLimit.h"

namespace gdl::game {
namespace {
bool finite(const Vec3& point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}
bool ordinaryAction(PlayerAnimator::Action action) {
    using Action = PlayerAnimator::Action;
    // Power swings and spins have separate authored target/effect semantics.
    // The strong-throw rows use the prepared native free-A phase below.
    const bool melee = PlayerAnimator::isMelee(action) && action != Action::Spin &&
                       action != Action::SpinRecover && action != Action::PowerClose &&
                       action != Action::PowerCloseRecover && action != Action::PowerMed &&
                       action != Action::PowerMedRecover && action != Action::PowerLow &&
                       action != Action::PowerLowRecover;
    return PlayerAnimator::locomotion(action) || PlayerAnimator::rollbackReactionAction(action) ||
           action == Action::Pushed || action == Action::Pick || melee ||
           (action >= Action::Throw && action <= Action::ThrowMovingRecover) ||
           (action >= Action::StrafeShootForward1 && action <= Action::StrafeShootRight2) ||
           action == Action::StrongThrow || action == Action::StrongThrowRecover ||
           action == PlayerAnimator::Action::DefendRaise ||
           action == PlayerAnimator::Action::Defend ||
           action == PlayerAnimator::Action::DefendLower;
}
bool ordinaryInput(const PlayInput& input) {
    return std::isfinite(input.move.direction.x) && std::isfinite(input.move.direction.y) &&
           std::abs(input.move.direction.x) <= 1 && std::abs(input.move.direction.y) <= 1 &&
           std::isfinite(input.move.magnitude) && input.move.magnitude >= 0 &&
           input.move.magnitude <= 1 && (!input.aimDirection || finite(*input.aimDirection)) &&
           !input.combo && !input.chargePressed &&
           !(input.turbo && (input.attack || input.attackPressed)) && !input.turboAttackPressed &&
           !input.usePotion && !input.throwPotion && !input.shieldPotion && !input.selector.up &&
           !input.selector.down && !input.selector.left && !input.selector.right;
}
bool quiet(const OpponentState& opponents) {
    const auto& enemies = opponents.enemies;
    const auto& missiles = opponents.missiles;
    return enemies.bursts.empty() && enemies.deathEvents.empty() && enemies.deathShots.empty() &&
           enemies.tagged.empty() && missiles.hits.empty() && missiles.rockHits.empty() &&
           missiles.pickupBlasts.empty() && missiles.bursts.empty() &&
           std::ranges::none_of(enemies.cues,
                                [](const auto& cue) { return cue.kind == EnemyCue::Kind::Yell; });
}
bool triggerOutputs(const LevelWorld& world) {
    const auto state = world.triggers().checkpoint();
    return !state || !state->refusals.empty() || !state->lessons.empty() ||
           !state->openings.empty() || !state->settled.empty() || !state->cameraCues.empty() ||
           !state->wakes.empty() || std::ranges::any_of(state->triggers, [](const auto& trigger) {
               return trigger.movementLesson;
           });
}
std::vector<TriggerVisitor> triggerVisitors(std::span<const PlayerRuntime> party,
                                            const WorldCollision& collision) {
    std::vector<TriggerVisitor> result;
    result.reserve(party.size());
    for (usize index = 0; index < party.size(); ++index) {
        // Admission guarantees every member is standing, uncaptured and present.
        // Keep PlayScene::visitors' pressure-pad reach, not the wall footprint.
        const auto& actor = party[index].actor;
        TriggerVisitor visitor;
        visitor.position = actor.position();
        visitor.radius = actor.reach();
        visitor.height = actor.height();
        visitor.crystals = actor.save().progress().crystals;
        visitor.gargoylePieces = actor.save().progress().relics.gargoylePieces;
        visitor.sumner = actor.save().character == kSumnerClass;
        visitor.party = static_cast<s32>(index);
        if (const auto floor = collision.floorAt(visitor.position, 0.5f, 1)) {
            visitor.floorObject = floor->object;
        }
        result.push_back(visitor);
    }
    return result;
}
class Undo {
public:
    Undo(OrdinaryCombatCheckpoint& owner, OrdinaryCombatCheckpoint::Rewind&& rewind)
        : m_owner(owner), m_rewind(std::move(rewind)) {}
    ~Undo() {
        if (m_rewind && !m_owner.rewind(std::move(*m_rewind))) {
            // No external callback runs in this scope. Failure indicates an
            // ownership contract violation, not a recoverable partial tick.
            std::terminate();
        }
    }
    void accept() { m_rewind.reset(); }

private:
    OrdinaryCombatCheckpoint& m_owner;
    std::optional<OrdinaryCombatCheckpoint::Rewind> m_rewind;
};
} // namespace

OrdinaryCombatTick::TargetFamily OrdinaryCombatTick::targetFamily(s32 target) {
    if (target < 0) {
        return TargetFamily::Unknown;
    }
    if (target >= PlayerAttacks::kPlayerTargetBase) {
        return TargetFamily::Player;
    }
    if (target >= PlayerAttacks::kStatueTargetBase) {
        return TargetFamily::Statue;
    }
    if (target >= PlayerAttacks::kChestTargetBase) {
        return TargetFamily::Chest;
    }
    if (target >= PlayerAttacks::kPotionTargetBase) {
        return TargetFamily::Potion;
    }
    if (target >= PlayerAttacks::kItemStopBase) {
        return TargetFamily::InertStop;
    }
    if (target >= PlayerAttacks::kSwitchTargetBase) {
        return TargetFamily::Switch;
    }
    if (target >= PlayerAttacks::kWallTargetBase) {
        return TargetFamily::Wall;
    }
    if (target >= PlayerAttacks::kSafeRockTargetBase) {
        return TargetFamily::SafeRock;
    }
    if (target >= PlayerAttacks::kBossTargetBase) {
        return TargetFamily::Boss;
    }
    if (target >= PlayerAttacks::kCritterTargetBase) {
        return TargetFamily::Critter;
    }
    if (target >= PlayerAttacks::kGeneratorTargetBase) {
        return TargetFamily::Generator;
    }
    return target >= PlayerAttacks::kEnemyTargetBase ? TargetFamily::Swarm : TargetFamily::Barrel;
}

OrdinaryCombatTick::OrdinaryCombatTick(OrdinaryCombatCheckpoint& checkpoint,
                                       const ClassDataSet& classes, LevelFixtures& fixtures)
    : m_checkpoint(checkpoint), m_classes(classes), m_fixtures(fixtures) {
    if (m_checkpoint.stageBound() && m_checkpoint.bindingsValid()) {
        const auto& world = m_checkpoint.m_opponents.m_resources->world;
        m_exits = ExitPortals::contacts(world.layout(), &world.collision());
        m_transports = LevelTransporters::contacts(
            world.layout(), static_cast<s32>(m_checkpoint.m_combat.m_players.size()),
            &world.collision());
    }
}

bool OrdinaryCombatTick::Frame::valid() const {
    return ticks >= 1 && ticks <= 2 && std::isfinite(seconds) &&
           std::abs(seconds * 60 - static_cast<f32>(ticks)) <= 0.0001f && finite(camera.position) &&
           std::isfinite(camera.pitch) && std::isfinite(camera.yaw) && std::isfinite(camera.roll) &&
           finite(attention) && std::isfinite(projection.horizontalFov) &&
           projection.horizontalFov > 0.01f && projection.horizontalFov < 3.13f &&
           std::isfinite(projection.aspect) && projection.aspect > 0.01f &&
           projection.aspect <= 32 && damage.valid() && std::isfinite(damage.playSeconds + seconds);
}

bool OrdinaryCombatTick::bound(std::span<PlayerRuntime> party) const {
    const auto& combat = m_checkpoint.m_combat;
    if (!m_checkpoint.bindingsValid() || party.empty() || party.size() != combat.m_players.size() ||
        !m_fixtures.m_resources) {
        return false;
    }
    const auto& resources = *m_checkpoint.m_opponents.m_resources;
    const auto& fixtures = *m_fixtures.m_resources;
    if (!combat.m_attacks.m_resources || !combat.m_arsenal.m_resources ||
        &combat.m_attacks.m_resources->classes != &m_classes ||
        &combat.m_arsenal.m_resources->classes != &m_classes ||
        &fixtures.world != &resources.world || &fixtures.effects != &resources.effects ||
        &fixtures.weapons != &resources.weapons || &fixtures.device != &resources.device ||
        (m_checkpoint.stageBound() && !m_checkpoint.m_fixtures->binds(m_fixtures))) {
        return false;
    }
    for (usize index = 0; index < party.size(); ++index) {
        const auto* owner = combat.m_players[index];
        if (owner == nullptr || &owner->m_runtime != &party[index] || owner->m_stats == nullptr ||
            owner->m_stats != m_classes.stats(party[index].actor.save().character)) {
            return false;
        }
    }
    return true;
}

bool OrdinaryCombatTick::environment() const {
    const auto& world = m_checkpoint.m_opponents.m_resources->world;
    // These scenes can start a hold/travel on the next tick without a contact:
    // tower congratulations/visits and the secret challenge deadline. Their
    // scene owners are not participants in this transaction yet.
    if (world.isTower() || world.ref().isSecret()) {
        return false;
    }
    if (m_checkpoint.stageBound()) {
        // These owners are complete participants, not omitted frozen rosters.
        // Actual contacts/outputs are checked at their native phase below.
        return world.collision().loaded() && !triggerOutputs(world);
    }
    // These are the actual bound owners, not an empty obstacle list supplied by
    // the caller. Their updates may spend keys, produce drops, or mutate world
    // geometry, none of which belongs to this combat transaction yet.
    return m_fixtures.m_chests.size() == 0 && m_fixtures.m_gates.size() == 0 &&
           m_fixtures.m_traps.size() == 0 && m_fixtures.m_barrels.size() == 0 &&
           m_fixtures.m_safeRocks.size() == 0 && m_fixtures.m_scenery.size() == 0 &&
           m_fixtures.m_rubble.size() == 0 && m_fixtures.m_clouds.empty() &&
           m_fixtures.m_blasts.empty() && m_fixtures.m_doomedChests.empty() &&
           !world.hasWorldExplosions() && !world.triggers().hasWakes() &&
           world.collision().loaded() && world.collision().movingObjectCount() == 0 &&
           world.hazards().harmful() == 0 && world.fallingScenery().size() == 0 &&
           world.walls().size() == 0 && world.placedItems().size() == 0;
}

bool OrdinaryCombatTick::travelContact(std::span<const PlayerRuntime> party, bool portals) const {
    const auto& world = m_checkpoint.m_opponents.m_resources->world;
    if (!portals) {
        if (m_checkpoint.m_transporters != nullptr) {
            const auto& transporters = m_checkpoint.m_transporters->m_owner;
            return std::ranges::any_of(party, [&](const auto& runtime) {
                const auto& actor = runtime.actor;
                return transporters.contact(actor.position(), actor.radius(), actor.height())
                    .has_value();
            });
        }
        return std::ranges::any_of(m_transports, [&](auto pad) {
            pad.sync(world.collision());
            return std::ranges::any_of(party, [&](const auto& runtime) {
                const auto& actor = runtime.actor;
                return pad.touches(actor.position(), actor.radius(), actor.height());
            });
        });
    }
    for (auto exit : m_exits) {
        exit.sync(&world.collision());
        if (exit.secret && !shownToParty(exit.minPlayers, static_cast<s32>(party.size()))) {
            continue;
        }
        const f32 extra = exit.secret || party.empty() ? 0 : static_cast<f32>(party.size() - 1);
        for (const auto& runtime : party) {
            const auto& actor = runtime.actor;
            if (exit.touches({actor.position(), actor.radius()}, extra)) {
                return true;
            }
        }
    }
    return false;
}

bool OrdinaryCombatTick::supported(const OrdinaryCombatCheckpoint::State& state,
                                   std::span<const PlayerRuntime> party, f32 seconds) const {
    if (!OrdinaryStrongPhase::supported(m_checkpoint.m_combat.m_attacks, party) ||
        !OrdinaryProjectilePhase::supported(state.combat.arsenal, m_checkpoint.m_combat.m_missiles,
                                            seconds) ||
        !quiet(state.opponents) || !OrdinaryCritterPhase::supported(state.adapter) ||
        std::ranges::any_of(state.adapter.lessons,
                            [&](const auto& lesson) {
                                return !PartyHelpAdmission::suppressedForPlayer(
                                    lesson.first, lesson.second, party);
                            }) ||
        !state.adapter.rockHits.empty() || state.adapter.shrink != 1 ||
        state.opponents.enemies.shrink != 1 || state.opponents.missiles.shrink != 1 ||
        std::ranges::any_of(state.combat.effects.effects, [&](const auto& effect) {
            if (std::ranges::find(state.adapter.cueEffects, effect.id) !=
                state.adapter.cueEffects.end()) {
                return false;
            }
            const auto& resources = m_checkpoint.m_combat.m_sharedEffects;
            const auto resource =
                std::ranges::find(resources, effect.resource, &EffectTrees::PoolResource::id);
            return resource == resources.end() ||
                   (!OrdinaryStrongPhase::supportsEffect(m_checkpoint.m_combat.m_attacks, *resource,
                                                         party) &&
                    !OrdinaryProjectilePhase::supportsEffect(m_checkpoint.m_combat.m_arsenal,
                                                             *resource));
        })) {
        return false;
    }
    const auto& collision = m_checkpoint.m_opponents.m_resources->world.collision();
    const auto loaded = [&](s32 kind) {
        return std::ranges::any_of(state.opponents.enemies.stocks,
                                   [&](const auto& stock) { return stock.kind == kind; });
    };
    // watch() may activate placements before the pool step. Every possible
    // birth must already have trusted artwork; loading mid-transaction would
    // invalidate the preallocated undo and could allocate graphics resources.
    if (!state.opponents.placements ||
        std::ranges::any_of(state.opponents.placements->roster,
                            [&](const auto& row) {
                                return row.kind >= 0 && row.kind < kSwarmKindCount &&
                                       row.kind != kGarmBroodKind && !loaded(row.spawn.kind);
                            }) ||
        std::ranges::any_of(state.opponents.generators.generators,
                            [&](const auto& row) { return row.state > 0 && !loaded(row.kind); })) {
        return false;
    }
    for (const auto& player : party) {
        if (player.life != PlayerLife::Standing || player.departed || player.figure == nullptr ||
            player.capture.active() || player.transport.active() || player.combo.active() ||
            player.itTicks != 0 || player.deathHeld != -1 ||
            (player.gesture != PlayerDeed::None && player.gesture != PlayerDeed::Pick) ||
            player.mikey.shown() || player.mikeyFigure != nullptr || player.gagSeconds != 0 ||
            player.figure->animator().entering() ||
            !ordinaryAction(player.figure->animator().action()) ||
            !PlayerAnimator::rollbackReactionDeed(player.reaction) ||
            player.figure->familiarTier() != 0 ||
            player.figure->companion().kind() != PowerupCompanion::Kind::None ||
            !OrdinaryFloorSupport::accepts(player, collision,
                                           OrdinaryFloorSupport::Phase::BeforeCarry,
                                           m_checkpoint.stageBound())) {
            return false;
        }
        const auto& inventory = player.actor.save().progress().inventory;
        if (std::ranges::any_of(inventory.powerups,
                                [](const auto& slot) { return slot.working(); })) {
            return false;
        }
        const auto impulse = player.knockback.checkpoint();
        if ((impulse.flags & ~Knockback::kKnockBack) != 0) {
            return false;
        }
        const auto move = player.move.checkpoint();
        const bool strong =
            player.figure->animator().action() == PlayerAnimator::Action::StrongThrow;
        const auto* stats = m_classes.stats(player.actor.save().character);
        if (move.owed != 0 || move.weaponHidden ||
            (strong ? stats == nullptr || move.all != stats->strikesOf(stats->moves.turboAThrow)
                    : !move.pending.empty() || !move.all.empty())) {
            return false;
        }
    }
    return true;
}

OrdinaryCombatTick::Result OrdinaryCombatTick::step(std::span<PlayerRuntime> party,
                                                    std::span<const PlayInput> inputs,
                                                    const Frame& suppliedFrame) {
    Timings timings;
    auto result = stepTimed(party, inputs, suppliedFrame, timings, nullptr, nullptr);
    result.timings = timings;
    return result;
}

OrdinaryCombatTick::Result OrdinaryCombatTick::stepUnderRewind(
    std::span<PlayerRuntime> party, std::span<const PlayInput> inputs, const Frame& frame,
    const OrdinaryCombatCheckpoint::Rewind& rewind, Boundary& boundary) {
    Timings timings;
    auto result = stepTimed(party, inputs, frame, timings, &rewind, &boundary);
    result.timings = timings;
    return result;
}

OrdinaryCombatTick::Result OrdinaryCombatTick::stepTimed(
    std::span<PlayerRuntime> party, std::span<const PlayInput> inputs, const Frame& suppliedFrame,
    Timings& timings, const OrdinaryCombatCheckpoint::Rewind* intervalUndo, Boundary* boundary) {
    using Clock = std::chrono::steady_clock;
    const auto elapsed = [](Clock::time_point begin) {
        return static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
    };
    if (intervalUndo && !m_checkpoint.canRewind(*intervalUndo)) {
        return {};
    }
    if (boundary && (!intervalUndo || (boundary->state && (boundary->owner != this ||
                                                           boundary->rewind != intervalUndo)))) {
        return {};
    }
    Frame frame = suppliedFrame;
    // Gameplay camera state is part of the joint correction. Its smoothing
    // history advances each native tick; repeating the seed's visible pose
    // would change camera-relative controls and multiplayer movement limits.
    if (m_checkpoint.m_camera && m_checkpoint.bindingsValid()) {
        frame.camera = m_checkpoint.m_camera->m_owner.camera();
        frame.attention = m_checkpoint.m_camera->m_owner.attention();
    }
    if (!frame.valid() || inputs.size() > 4 || !bound(party)) {
        return {};
    }
    if (frame.damage.bossEncounter || frame.damage.cutscene || !environment() ||
        m_checkpoint.m_combat.m_attacks.m_resources->dimmer.active() ||
        std::ranges::any_of(inputs, [](const auto& input) { return !ordinaryInput(input); })) {
        return {Status::Unsupported, Refusal::Environment};
    }
    const bool capture = boundary == nullptr || !boundary->state;
    const auto captureBegin = Clock::now();
    const auto capturedBefore = capture ? m_checkpoint.checkpoint() : std::nullopt;
    if (capture) {
        timings.captureNanoseconds = elapsed(captureBegin);
    }
    const OrdinaryCombatCheckpoint::State* before = nullptr;
    if (capture) {
        if (capturedBefore) {
            before = &*capturedBefore;
        }
    } else {
        before = boundary->state.get();
    }
    if (!before) {
        return {};
    }
    if (before->stage && before->stage->world.ambientOffset != 0) {
        return {Status::Unsupported, Refusal::Ambient};
    }
    if (!supported(*before, party, frame.seconds)) {
        return {Status::Unsupported, Refusal::Admission};
    }
    const ViewVolume view =
        ViewVolume::of(frame.camera, frame.projection.horizontalFov, frame.projection.aspect);
    const auto activations = before->opponents.placements->activations(view, frame.attention);
    if (!activations || std::ranges::any_of(*activations, [](const auto& placement) {
            return placement.kind < 0 || placement.kind >= kSwarmKindCount ||
                   placement.kind == kGarmBroodKind || placement.carried;
        })) {
        return {Status::Unsupported, Refusal::Placements};
    }
    const auto dormantStatues = [&] {
        std::vector<CritterStatues::Visitor> visitors;
        visitors.reserve(party.size());
        for (const auto& player : party) {
            visitors.push_back(
                {player.actor.position(), player.actor.radius(), player.actor.height()});
        }
        return m_checkpoint.m_opponents.m_statues.dormantFor(visitors);
    };
    if (!dormantStatues()) {
        return {Status::Unsupported, Refusal::Statues};
    }
    // Native watch below installs this frame's observation before activating
    // placements. Probe it without mutating the immutable preceding boundary.
    const OpponentSimulation::PlacementObservation observation{view, frame.attention};
    const auto initialViews = LevelOpponents::enemyViews(party);
    const auto initialCritters = m_checkpoint.m_opponents.m_critters.targets(true);
    if (!OpponentSimulation::combatSupported(before->opponents,
                                             {.ticks = frame.ticks,
                                              .seconds = frame.seconds,
                                              .players = initialViews,
                                              .critters = initialCritters},
                                             &observation)) {
        return {Status::Unsupported, Refusal::OpponentAdmission};
    }
    std::optional<Undo> undo;
    if (intervalUndo == nullptr) {
        const auto prepareBegin = Clock::now();
        auto rewind = m_checkpoint.prepareRewind();
        timings.prepareNanoseconds = elapsed(prepareBegin);
        if (!rewind) {
            return {};
        }
        undo.emplace(m_checkpoint, std::move(*rewind));
    }
    auto& combat = m_checkpoint.m_combat;
    auto& opponents = m_checkpoint.m_opponents;
    auto& world = opponents.m_resources->world;
    const auto& collision = world.collision();
    Result result;
    bool refused = false;
    PartyRefusal partyRefusal = PartyRefusal::None;
    std::optional<PlayerAnimator::Action> refusedAction;
    TargetFamily refusedTarget = TargetFamily::None;
    const auto refuseParty = [&](PartyRefusal reason,
                                 std::optional<PlayerAnimator::Action> action = std::nullopt,
                                 TargetFamily target = TargetFamily::None) {
        refused = true;
        if (partyRefusal == PartyRefusal::None) {
            partyRefusal = reason;
            refusedAction = action;
            refusedTarget = target;
        }
    };
    const bool staged = m_checkpoint.stageBound();
    if (staged) {
        PartyFigures::snapshot(party);
        combat.m_effects.capturePresentation();
        world.capturePresentation();
        m_fixtures.capturePresentation();
        if (m_checkpoint.m_transporters != nullptr) {
            m_checkpoint.m_transporters->m_owner.capturePresentation();
        }
        PartyNames::advance(party, frame.ticks);
        // These native clocks can change even when the current equipment mask
        // is empty (an item just expired or a melee streak is awaiting praise).
        // Keep their value changes under the same undo, but never publish an
        // ending, narrator line or newly allocated companion here.
        if (!PlayerPowerups::update(party, frame.seconds,
                                    frame.damage.tower ? PlayerPowerups::Clock::Paused
                                                       : PlayerPowerups::Clock::Level)
                 .empty()) {
            return {Status::Unsupported, Refusal::Powerups};
        }
        for (auto& player : party) {
            if (!player.streak.step(frame.seconds).empty() || player.gem.update(0) ||
                player.mikey.shown()) {
                return {Status::Unsupported, Refusal::PlayerTimers};
            }
        }
        // Active item clocks/scene holds are excluded by admission. These are
        // the native value-only world phases before transporter/player motion.
        world.update(frame.seconds, false);
        opponents.syncFloors();
        if (m_checkpoint.m_transporters != nullptr) {
            m_checkpoint.m_transporters->m_owner.syncFloors(collision);
        }
        world.revealCrystals(frame.seconds);
        if (travelContact(party, false) || std::ranges::any_of(party, [&](const auto& player) {
                return !OrdinaryFloorSupport::accepts(
                    player, collision, OrdinaryFloorSupport::Phase::BeforeCarry, staged);
            })) {
            return {Status::Unsupported, Refusal::WorldMotion};
        }
        for (auto& player : party) {
            player.transport.observeContact(false);
        }
        if (m_checkpoint.m_transporters != nullptr) {
            m_checkpoint.m_transporters->m_owner.animate(frame.seconds);
        }
    }
    const PlayerAttacks::Targets targets{
        .opponents = opponents,
        .fixtures = m_fixtures,
        .fixtureEvents = {},
        .players = party,
        .meleeHit = [&](s32 player) { result.meleeHits.push_back(player); }};
    const PartyMotion::Events events{
        .perform =
            [&](usize index, PartyMotion::Action action) {
                if (action == PartyMotion::Action::FirstFoot ||
                    action == PartyMotion::Action::SecondFoot) {
                    const auto& player = party[index];
                    const auto& actor = player.actor;
                    const auto water = collision.liquidAt(actor.position(), actor.height(), 0);
                    result.footsteps.push_back(
                        {actor.player(), action,
                         LevelSoundscape::footingOf(
                             player.floor.flags,
                             PowerupEffects::of(actor.save().progress().inventory).armor,
                             water && water->y > actor.position().y)});
                } else if (action == PartyMotion::Action::Melee) {
                    const auto& player = party[index];
                    // Native melee queries SHOOTFALL before applying the winning
                    // blow. The leaf is rewindable; its audio dispatch is not.
                    if (staged && !world.fallingScenery()
                                       .shoot(player.actor.position(),
                                              player.actor.reach() + PlayerAttacks::kStepReach)
                                       .empty()) {
                        refuseParty(PartyRefusal::MeleeScenery);
                        return;
                    }
                    // Check the exact native winning contact, after resolved
                    // movement/aim. A nearby ordinary enemy must not hide a
                    // selected special generator, statue, player or other world item.
                    const auto target = combat.m_attacks.meleeTarget(
                        player.actor, targets, player.meleeFacing.value_or(player.actor.facing()),
                        player.actor.reach() + PlayerAttacks::kStepReach);
                    const bool barrel = target && target->body.id >= 0 &&
                                        target->body.id < PlayerAttacks::kEnemyTargetBase;
                    if (target && (target->body.id < 0 ||
                                   (barrel && (!staged || !m_fixtures.m_blasts.empty() ||
                                               static_cast<usize>(target->body.id) >=
                                                   m_fixtures.barrels().size())) ||
                                   target->body.id >= PlayerAttacks::kCritterTargetBase ||
                                   (target->body.id >= PlayerAttacks::kGeneratorTargetBase &&
                                    !OrdinaryProjectilePhase::ordinaryGenerator(
                                        opponents.generators(),
                                        target->body.id - PlayerAttacks::kGeneratorTargetBase)))) {
                        refuseParty(PartyRefusal::MeleeTarget, player.figure->animator().action(),
                                    targetFamily(target->body.id));
                    } else if (!refused) {
                        if (!combat.m_attacks.melee(
                                index, party, targets,
                                [&](const GeneratorFeedbackEmission& emission) {
                                    return OrdinaryProjectilePhase::generatorPresentation(
                                        emission, combat.m_effects,
                                        combat.m_attacks.m_resources->device,
                                        combat.m_sharedEffects, result.impacts);
                                },
                                [&](const BreakableStrike& strike, std::string_view sound) {
                                    // Native strike already resolved damage, contents and
                                    // RNG. Publish a prepared birth in that same transaction;
                                    // the later collector still decides actual pickup eligibility.
                                    const auto emission = OrdinaryBarrelEmission::advance(
                                        strike, sound, player.actor.player(), party, m_fixtures,
                                        *m_checkpoint.m_world);
                                    if (!emission.accepted) {
                                        auto reason = PartyRefusal::BarrelEmission;
                                        if (strike.contents >= 0) {
                                            reason = PartyRefusal::BarrelContents;
                                        } else if (strike.kind ==
                                                   BreakableStrike::Kind::Exploding) {
                                            reason = PartyRefusal::BarrelExplosion;
                                        } else if (strike.kind == BreakableStrike::Kind::Poison) {
                                            reason = PartyRefusal::BarrelGas;
                                        }
                                        refuseParty(reason, player.figure->animator().action(),
                                                    TargetFamily::Barrel);
                                        return false;
                                    }
                                    if (emission.cue) {
                                        result.impacts.push_back(*emission.cue);
                                    }
                                    return true;
                                })) {
                            refuseParty(barrel ? PartyRefusal::BarrelEmission
                                               : PartyRefusal::GeneratorEmission,
                                        player.figure->animator().action(),
                                        barrel ? TargetFamily::Barrel : TargetFamily::Generator);
                        }
                    }
                } else if ((action == PartyMotion::Action::ThrowWeapon ||
                            action == PartyMotion::Action::StrongThrow) &&
                           !refused) {
                    auto& player = party[index];
                    const auto& actor = player.actor;
                    const auto target = player.cursorAiming
                                            ? std::nullopt
                                            : combat.m_attacks.aim(actor, actor.facing(), targets);
                    const auto emission = OrdinaryWeaponEmission::advance(
                        combat.m_arsenal, actor, *player.figure, actor.facing(), target,
                        {combat.m_missiles, combat.m_missileEffects, combat.m_sharedEffects});
                    if (!emission.accepted) {
                        refuseParty(PartyRefusal::WeaponEmission);
                    } else {
                        if (emission.cue) {
                            result.throws.push_back(*emission.cue);
                        }
                        if (emission.impact) {
                            result.impacts.push_back(*emission.impact);
                        }
                    }
                } else if (action == PartyMotion::Action::FamiliarShot) {
                    const auto& player = party[index];
                    const auto worn = PowerupEffects::of(player.actor.save().progress().inventory);
                    // A just-expired Phoenix can leave the previous tick's
                    // familiarPending latch. Native launchFamiliar returns
                    // before any work when neither actual shooter remains.
                    if ((worn.special & powerup::kPhoenix) != 0 ||
                        (player.figure->familiarTier() > 0 &&
                         player.figure->familiarMissile().bound())) {
                        refuseParty(PartyRefusal::FamiliarShot);
                    }
                } else if (action == PartyMotion::Action::NoPotion) {
                    // This posts real help/narration, never an empty callback.
                    refuseParty(PartyRefusal::NoPotion);
                } else {
                    refuseParty(PartyRefusal::UnsupportedAction);
                }
            },
        .select =
            [&](usize, const SelectorInput& selector, s32) {
                // A neutral selector has no gameplay work. HUD selection clocks are
                // outside this phase; nonneutral commands were rejected before it.
                if (selector.up || selector.down || selector.left || selector.right) {
                    refuseParty(PartyRefusal::Selector);
                }
            },
        .advanceTurbo =
            [&](usize index, s32 ticks, f32 seconds) {
                const auto action = party[index].figure->animator().action();
                if (!ordinaryAction(action)) {
                    refuseParty(PartyRefusal::TurboAction, action);
                    return;
                }
                const auto help = [&](s32 id, usize namedPlayer) {
                    // PlayScene filters meter lessons before PartyHud looks up
                    // the retail message. Unknown free-A IDs really do nothing.
                    const bool crowd = opponents.enemies().inView() >= 15;
                    if ((id == HelpMessages::kUseCombo && (!crowd || party.size() <= 1)) ||
                        (id == HelpMessages::kUseTurbo && !crowd)) {
                        return true;
                    }
                    const bool supported = PartyHelpAdmission::suppressed(id, namedPlayer, party);
                    if (!supported) {
                        refuseParty(PartyRefusal::TurboLesson);
                    }
                    return supported;
                };
                if (action == PlayerAnimator::Action::StrongThrow ||
                    action == PlayerAnimator::Action::StrongThrowRecover) {
                    auto emitted = OrdinaryStrongPhase::advance(
                        combat.m_attacks, index, ticks, seconds, party,
                        {combat.m_missiles, combat.m_missileEffects, combat.m_sharedEffects}, help);
                    if (!emitted.advanced) {
                        refuseParty(PartyRefusal::TurboEmission);
                    }
                    result.strong.insert(result.strong.end(),
                                         std::make_move_iterator(emitted.cues.begin()),
                                         std::make_move_iterator(emitted.cues.end()));
                } else {
                    combat.m_attacks.updateTurbo(
                        index, ticks, seconds, party,
                        [&](s32 id, usize actor) { (void)help(id, actor); });
                }
            },
        .aim =
            [&](usize index) {
                const auto& actor = party[index].actor;
                return combat.m_attacks.aim(actor, actor.facing(), targets);
            },
        .limitMovement =
            [&](usize index, const Vec3& from, const Vec3& to) {
                return party.size() == 1
                           ? to
                           : CameraMovementLimit::constrain(
                                 from, to, frame.attention, frame.camera, frame.projection,
                                 party[index].actor.followPoint() - party[index].actor.position());
            },
        .attackDeed =
            [&](usize index, bool strong, bool moved, const Vec3& facing) {
                const auto& player = party[index];
                // An existing swing may receive the ranged deed after its target
                // disappears and still enter ordinary recovery. Preserve that
                // native decision; advanceTurbo refuses unsupported special
                // actions before any release callback can touch the arsenal.
                return combat.m_attacks.attackDeed(player.actor, strong, targets, moved,
                                                   player.figure->animator().meleeChain(), facing);
            },
        .automaticMeleeDeed =
            [&](usize index, const Vec3& facing) {
                return frame.autoMelee ? combat.m_attacks.automaticMeleeDeed(party[index].actor,
                                                                             targets, facing)
                                       : PlayerDeed::None;
            },
        .meleeSense =
            [&](usize index, bool held, const Vec3& facing) {
                return combat.m_attacks.meleeSense(party[index].actor, held, targets, facing);
            },
        .resolveMovement =
            [&](usize index, const Vec3& from, const Vec3& to) {
                return opponents.resolveMovement(party[index].actor, from, to);
            }};
    result.subjects = PartyMotion::step(party, inputs, false, frame.camera.yaw, frame.ticks,
                                        frame.seconds, collision, events);
    if (!refused && !dormantStatues()) {
        refuseParty(PartyRefusal::Statue);
    }
    if (!refused && std::ranges::any_of(party, [&](const auto& player) {
            return !OrdinaryFloorSupport::accepts(
                player, collision, OrdinaryFloorSupport::Phase::AfterLanding, staged);
        })) {
        refuseParty(PartyRefusal::FloorSupport);
    }
    if (refused) {
        return {.status = Status::Unsupported,
                .refusal = Refusal::PartyMotion,
                .partyRefusal = partyRefusal,
                .refusedAction = refusedAction,
                .refusedTarget = refusedTarget};
    }
    PartyRecords::advanceTime(party, frame.seconds);
    if (staged) {
        if (OrdinaryFixturePhase::advance(m_fixtures, frame.ticks, frame.seconds, party).refusal ||
            !dormantStatues() || std::ranges::any_of(party, [&](const auto& player) {
                return !OrdinaryFloorSupport::accepts(
                    player, collision, OrdinaryFloorSupport::Phase::AfterLanding, staged);
            })) {
            return {Status::Unsupported, Refusal::Fixtures};
        }
    } else {
        for (auto& player : party) {
            LevelFixtures::observePlayer(player, player.actor.position());
        }
        LevelFixtures::advanceHitSounds(party, frame.ticks);
    }
    for (auto& player : party) {
        PlayerHealth::advanceSurfaceGap(player, frame.seconds);
        if (staged && player.surfaceGap <= 0 && PlayerHealth::canTakeSurfaceDamage(player) &&
            world.hazards().touching(collision, player.actor.position(), player.actor.radius(),
                                     player.actor.height(), player.actor.wallContacts())) {
            return {Status::Unsupported, Refusal::Surface};
        }
    }
    result.heartbeat = PlayerHealth::heartbeat(party, frame.ticks, frame.damage.tower);
    opponents.watch(view, frame.attention);
    opponents.beginOrdinaryFrame(frame.seconds, party);
    opponents.countOrdinaryPlayers(party);
    const auto views = LevelOpponents::enemyViews(party);
    const auto* level = world.level();
    const auto obstacles = m_fixtures.obstacles();
    const auto stops = m_fixtures.missileStops();
    const auto critters = opponents.m_critters.targets(true);
    opponents.m_enemies.setCombatantBodies(critters);
    const OpponentSimulation::Frame opponentFrame{
        .ticks = frame.ticks,
        .seconds = frame.seconds,
        .players = views,
        .fixtures = obstacles,
        .stops = stops,
        .critters = critters,
        .collision = &collision,
        .missileSpeed = level != nullptr ? level->tuning.enemyMissileSpeed : 1};
    if (m_checkpoint.m_pools.stageCombat(opponentFrame, m_checkpoint.m_missiles, [&] {
            return opponents.playBirths(
                [&](const EnemyBirthEmission& emission) -> std::optional<u32> {
                    const auto prepared =
                        emission.prepare(combat.m_sharedEffects, combat.m_effects.textureLenders());
                    if (!prepared) {
                        return std::nullopt;
                    }
                    const auto effect =
                        combat.m_effects.startPrepared(*prepared, emission.position, {});
                    return effect != 0 ? std::optional{effect} : std::nullopt;
                });
        }) != OpponentSimulation::Result::Advanced) {
        return {Status::Unsupported, Refusal::OpponentStep};
    }
    result.enemyCues = opponents.m_enemies.takeCues();
    if (std::ranges::any_of(result.enemyCues,
                            [](const auto& cue) { return cue.kind == EnemyCue::Kind::Yell; })) {
        return {Status::Unsupported, Refusal::EnemyCue};
    }
    auto critterStep = OrdinaryCritterPhase::advance(opponents, frame.ticks, frame.seconds, views,
                                                     m_fixtures.critterObstacles(), party);
    if (!critterStep.advanced()) {
        return {Status::Unsupported, Refusal::OpponentStep};
    }
    auto damage = frame.damage;
    damage.playSeconds += frame.seconds;
    auto critterFinish = OrdinaryCritterPhase::finish(
        opponents, party, combat.m_health, m_classes, damage, combat.m_sharedEffects,
        [&](const Vec3& from, const Vec3& to) {
            return m_fixtures.safeRocks().blocksBreath(from, to);
        },
        [&](const Vec3& from, const Vec3& to) {
            return m_fixtures.safeRocks().blocksSegment(from, to, .1f);
        });
    if (!critterFinish.advanced()) {
        return {Status::Unsupported, Refusal::Damage};
    }
    for (auto& cue : critterFinish.sounds) {
        result.feedback.push_back({std::move(cue.name), cue.position, cue.volume});
    }
    result.damage = std::move(critterFinish.damage);
    // Native levels precede the ordinary blow queue. A new level needs healing,
    // equipment rebinding and lesson owners; refuse rather than emulate those.
    for (const auto& player : party) {
        if (combat.m_levels.observe(player.actor.player(),
                                    experienceLevel(player.actor.save().experience()))) {
            return {Status::Unsupported, Refusal::Promotion};
        }
    }
    auto blows =
        PartyDamage::stageBlows(opponents.m_enemies.takeBlows(), party, combat.m_health, m_classes,
                                level != nullptr ? std::span<const LevelEnemy>{level->enemies}
                                                 : std::span<const LevelEnemy>{},
                                damage);
    if (blows.status != PartyDamage::Status::Applied) {
        return {Status::Unsupported, Refusal::Damage};
    }
    result.damage.insert(result.damage.end(), std::make_move_iterator(blows.outputs.begin()),
                         std::make_move_iterator(blows.outputs.end()));
    // Native melee lands before the opponent phase; its feedback and experience
    // are delivered here, after incoming blows and the level observation. Later
    // projectile impacts retain their queues until the next opponent phase.
    if (std::ranges::any_of(opponents.m_lessons, [&](const auto& lesson) {
            return !PartyHelpAdmission::suppressedForPlayer(lesson.first, lesson.second, party);
        })) {
        return {Status::Unsupported, Refusal::Lessons};
    }
    // Drain the native queue in order only after proving every routed HUD call
    // is suppressed by its repeat policy. No new lesson is silently discarded.
    opponents.deliverLessons(party, {.help = [](s32, usize) { return false; }});
    const LevelOpponents::Events rewards{
        .award =
            [&](s32 player, s32 experience, bool killed) {
                PartyRecords::award(party, player, experience, killed, level);
            },
        .destroyedGenerator = [&](s32 player) { PartyRecords::destroyedGenerator(party, player); }};
    if (!opponents.awardEnemyLosses(
            rewards, [&](const EnemyFeedbackEmission& emission) -> std::optional<u32> {
                u32 effect = 0;
                if (emission.archive != nullptr) {
                    const auto prepared =
                        emission.prepare(combat.m_sharedEffects, combat.m_effects.textureLenders());
                    if (!prepared) {
                        return std::nullopt;
                    }
                    effect = combat.m_effects.startPrepared(*prepared, emission.position,
                                                            emission.setting);
                    if (effect == 0) {
                        return std::nullopt;
                    }
                }
                if (!emission.sound.empty()) {
                    constexpr f32 kLoudVolume = 224.0f / 255;
                    const f32 heard =
                        LevelOpponents::attenuation(emission.position, opponents.m_hearers);
                    if (heard > 0) {
                        result.feedback.push_back(
                            {emission.sound, emission.position, kLoudVolume * heard});
                    }
                }
                return effect;
            })) {
        return {Status::Unsupported, Refusal::Feedback};
    }
    for (usize index = 0; index < party.size(); ++index) {
        if (experienceLevel(party[index].actor.save().experience()) !=
            experienceLevel(before->combat.players[index].save.experience())) {
            return {Status::Unsupported, Refusal::RewardPromotion};
        }
    }
    // Missiles fly after opponent rewards, so their new hit/loss/feedback rows
    // remain queued for the NEXT opponent phase. The helper checks the complete
    // native impact batch before dispatching any external world interaction.
    auto projectiles = OrdinaryProjectilePhase::advance(
        combat.m_attacks, party, targets, frame.seconds,
        {combat.m_missiles, combat.m_missileEffects, combat.m_sharedEffects},
        staged ? m_checkpoint.m_world : nullptr);
    if (!projectiles.advanced) {
        Result rejected{Status::Unsupported, Refusal::Projectiles};
        rejected.projectileRefusal = projectiles.refusal;
        if (projectiles.refusedTarget) {
            rejected.refusedTarget = targetFamily(*projectiles.refusedTarget);
        }
        return rejected;
    }
    result.impacts.insert(result.impacts.end(), std::make_move_iterator(projectiles.cues.begin()),
                          std::make_move_iterator(projectiles.cues.end()));
    combat.m_attacks.updatePotions(frame.seconds, party, targets);
    combat.m_attacks.updateItems(frame.seconds, party, targets);
    if (!OrdinaryStrongPhase::stepStrikes(combat.m_attacks, frame.seconds, party, targets)) {
        return {Status::Unsupported, Refusal::Strikes};
    }
    combat.m_attacks.updateShields(frame.seconds, party, targets);
    combat.m_attacks.updateArmour(frame.seconds, party, targets);
    combat.m_effects.update(frame.seconds);
    if (staged) {
        auto pickups = OrdinaryPickupPhase::advance(party, world, m_fixtures, m_classes,
                                                    *m_checkpoint.m_world);
        if (!pickups.advanced) {
            return {Status::Unsupported, Refusal::Pickups};
        }
        result.pickups = std::move(pickups.outputs);
        world.updateTriggers(frame.seconds, triggerVisitors(party, collision));
        m_fixtures.syncFloors();
        opponents.syncFloors();
        if (m_checkpoint.m_transporters != nullptr) {
            m_checkpoint.m_transporters->m_owner.syncFloors(collision);
        }
        // Inspect before any narration, sound, camera, statue wake or travel
        // dispatch. Removing queued rotator cues is covered by the stage undo.
        if (triggerOutputs(world) || !world.takeRotatorCues().empty() ||
            travelContact(party, true) || travelContact(party, false)) {
            return {Status::Unsupported, Refusal::Trigger};
        }
        if (m_checkpoint.m_portals) {
            std::vector<PortalVisitor> visitors;
            for (usize index = 0; index < party.size(); ++index) {
                const auto& player = party[index];
                const auto seat = static_cast<usize>(player.actor.player());
                visitors.push_back({player.actor.position(), player.actor.radius(),
                                    static_cast<s32>(index),
                                    seat >= inputs.size() || !inputs[seat].move.any()});
            }
            auto& portals = m_checkpoint.m_portals->m_owner;
            if (portals.update(frame.ticks, frame.seconds, visitors) ||
                !portals.takeWaiting().empty()) {
                return {Status::Unsupported, Refusal::Portal};
            }
        }
        for (auto& player : party) {
            if (player.exitWait.step(false, frame.ticks)) {
                return {Status::Unsupported, Refusal::ExitWait};
            }
        }
    }
    if (m_checkpoint.m_camera) {
        m_checkpoint.m_camera->m_owner.update(result.subjects, world.cameraMarkers(),
                                              world.cameraRange(), frame.projection, frame.seconds);
    }
    const auto postconditionBegin = Clock::now();
    // All reads of the preceding boundary are complete. Its world storage can
    // now be consumed without changing any native owner or the interval undo.
    auto* reusableWorld = boundary && boundary->state && boundary->state->stage
                              ? &boundary->state->stage->world
                              : nullptr;
    auto after = m_checkpoint.capture(&timings.postconditionOwners, &timings.postconditionWorld,
                                      reusableWorld);
    const bool accepted = after && supported(*after, party, frame.seconds);
    timings.postconditionNanoseconds = elapsed(postconditionBegin);
    if (!accepted) {
        return {Status::Unsupported, Refusal::Postcondition};
    }
    if (boundary) {
        // Moving the already validated image keeps the next admission exact;
        // no second capture or caller-authored state can enter this cache.
        boundary->state = std::make_unique<OrdinaryCombatCheckpoint::State>(std::move(*after));
        boundary->owner = this;
        boundary->rewind = intervalUndo;
    }
    result.status = Status::Advanced;
    if (undo) {
        undo->accept();
    }
    return result;
}

} // namespace gdl::game
