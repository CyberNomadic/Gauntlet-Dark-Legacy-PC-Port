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
#include "game/screens/OrdinaryFixturePhase.h"
#include "game/screens/OrdinaryFloorSupport.h"
#include "game/screens/OrdinaryProjectilePhase.h"
#include "game/screens/PartyFigures.h"
#include "game/screens/PartyNames.h"
#include "game/screens/PartyPickups.h"
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
           melee || (action >= Action::Throw && action <= Action::ThrowMovingRecover) ||
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
        !quiet(state.opponents) || !state.adapter.dormant() || !state.adapter.lessons.empty() ||
        !state.adapter.generatorRewards.empty() || !state.adapter.rockHits.empty() ||
        state.adapter.shrink != 1 || state.opponents.enemies.shrink != 1 ||
        state.opponents.missiles.shrink != 1 ||
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
            player.itTicks != 0 || player.deathHeld != -1 || player.gesture != PlayerDeed::None ||
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
    auto result = stepTimed(party, inputs, suppliedFrame, timings);
    result.timings = timings;
    return result;
}

OrdinaryCombatTick::Result OrdinaryCombatTick::stepTimed(std::span<PlayerRuntime> party,
                                                         std::span<const PlayInput> inputs,
                                                         const Frame& suppliedFrame,
                                                         Timings& timings) {
    using Clock = std::chrono::steady_clock;
    const auto elapsed = [](Clock::time_point begin) {
        return static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
    };
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
    const auto captureBegin = Clock::now();
    auto before = m_checkpoint.checkpoint();
    timings.captureNanoseconds = elapsed(captureBegin);
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
    // This local checkpoint is only an admission probe. Native watch below
    // installs this frame's observation before activating placements; the
    // previous frame's view must not veto an otherwise dormant special.
    before->opponents.placements->view = view;
    before->opponents.placements->attention = frame.attention;
    const auto initialViews = LevelOpponents::enemyViews(party);
    if (!OpponentSimulation::combatSupported(
            before->opponents,
            {.ticks = frame.ticks, .seconds = frame.seconds, .players = initialViews})) {
        return {Status::Unsupported, Refusal::OpponentAdmission};
    }
    const auto prepareBegin = Clock::now();
    auto rewind = m_checkpoint.prepareRewind();
    timings.prepareNanoseconds = elapsed(prepareBegin);
    if (!rewind) {
        return {};
    }
    Undo undo{m_checkpoint, std::move(*rewind)};
    auto& combat = m_checkpoint.m_combat;
    auto& opponents = m_checkpoint.m_opponents;
    auto& world = opponents.m_resources->world;
    const auto& collision = world.collision();
    Result result;
    bool refused = false;
    const bool staged = m_checkpoint.stageBound();
    if (staged) {
        PartyFigures::snapshot(party);
        combat.m_effects.capturePresentation();
        world.capturePresentation();
        m_fixtures.capturePresentation();
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
                        refused = true;
                        return;
                    }
                    // Check the exact native winning contact, after resolved
                    // movement/aim. A nearby ordinary enemy must not hide a
                    // selected generator, statue, player or other world item.
                    const auto target = combat.m_attacks.meleeTarget(
                        player.actor, targets, player.meleeFacing.value_or(player.actor.facing()),
                        player.actor.reach() + PlayerAttacks::kStepReach);
                    if (target && (target->body.id < PlayerAttacks::kEnemyTargetBase ||
                                   target->body.id >= PlayerAttacks::kGeneratorTargetBase)) {
                        refused = true;
                    } else if (!refused) {
                        combat.m_attacks.melee(index, party, targets);
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
                    if (!emission.launched) {
                        refused = true;
                    } else if (emission.cue) {
                        result.throws.push_back(*emission.cue);
                    }
                } else {
                    refused = true;
                }
            },
        .select =
            [&](usize, const SelectorInput& selector, s32) {
                // A neutral selector has no gameplay work. HUD selection clocks are
                // outside this phase; nonneutral commands were rejected before it.
                refused |= selector.up || selector.down || selector.left || selector.right;
            },
        .advanceTurbo =
            [&](usize index, s32 ticks, f32 seconds) {
                const auto action = party[index].figure->animator().action();
                if (!ordinaryAction(action)) {
                    refused = true;
                    return;
                }
                const auto help = [&](s32 id, usize) {
                    // PlayScene filters meter lessons before PartyHud looks up
                    // the retail message. Unknown free-A IDs really do nothing.
                    const bool crowd = opponents.enemies().inView() >= 15;
                    if ((id == HelpMessages::kUseCombo && (!crowd || party.size() <= 1)) ||
                        (id == HelpMessages::kUseTurbo && !crowd)) {
                        return true;
                    }
                    return HelpMessages::specOf(id) == nullptr;
                };
                if (action == PlayerAnimator::Action::StrongThrow ||
                    action == PlayerAnimator::Action::StrongThrowRecover) {
                    auto emitted = OrdinaryStrongPhase::advance(
                        combat.m_attacks, index, ticks, seconds, party,
                        {combat.m_missiles, combat.m_missileEffects, combat.m_sharedEffects}, help);
                    refused |= !emitted.advanced;
                    result.strong.insert(result.strong.end(),
                                         std::make_move_iterator(emitted.cues.begin()),
                                         std::make_move_iterator(emitted.cues.end()));
                } else {
                    combat.m_attacks.updateTurbo(
                        index, ticks, seconds, party,
                        [&](s32 id, usize actor) { refused |= !help(id, actor); });
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
    if (refused || !dormantStatues() || std::ranges::any_of(party, [&](const auto& player) {
            return !OrdinaryFloorSupport::accepts(
                player, collision, OrdinaryFloorSupport::Phase::AfterLanding, staged);
        })) {
        return {Status::Unsupported, Refusal::PartyMotion};
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
    const OpponentSimulation::Frame opponentFrame{
        .ticks = frame.ticks,
        .seconds = frame.seconds,
        .players = views,
        .fixtures = obstacles,
        .stops = stops,
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
    // The complete bound great-creature pool is unused, and no statue can
    // wake or correct a player. Its native phase still advances each shared
    // preloaded texture bank, even while there is no visible creature.
    opponents.m_critters.update(frame.ticks, frame.seconds, views, false, {}, &opponents.m_enemies);
    opponents.followCritterEffects(party);
    // Native levels precede the ordinary blow queue. A new level needs healing,
    // equipment rebinding and lesson owners; refuse rather than emulate those.
    for (const auto& player : party) {
        if (combat.m_levels.observe(player.actor.player(),
                                    experienceLevel(player.actor.save().experience()))) {
            return {Status::Unsupported, Refusal::Promotion};
        }
    }
    auto damage = frame.damage;
    damage.playSeconds += frame.seconds;
    auto blows =
        PartyDamage::stageBlows(opponents.m_enemies.takeBlows(), party, combat.m_health, m_classes,
                                level != nullptr ? std::span<const LevelEnemy>{level->enemies}
                                                 : std::span<const LevelEnemy>{},
                                damage);
    if (blows.status != PartyDamage::Status::Applied) {
        return {Status::Unsupported, Refusal::Damage};
    }
    result.damage = std::move(blows.outputs);
    // Native melee lands before the opponent phase; its feedback and experience
    // are delivered here, after incoming blows and the level observation. Later
    // projectile impacts retain their queues until the next opponent phase.
    if (!opponents.m_lessons.empty()) {
        return {Status::Unsupported, Refusal::Lessons};
    }
    const LevelOpponents::Events rewards{.award = [&](s32 player, s32 experience, bool killed) {
        PartyRecords::award(party, player, experience, killed, level);
    }};
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
        {combat.m_missiles, combat.m_missileEffects, combat.m_sharedEffects});
    if (!projectiles.advanced) {
        return {Status::Unsupported, Refusal::Projectiles};
    }
    result.impacts = std::move(projectiles.cues);
    combat.m_attacks.updatePotions(frame.seconds, party, targets);
    combat.m_attacks.updateItems(frame.seconds, party, targets);
    if (!OrdinaryStrongPhase::stepStrikes(combat.m_attacks, frame.seconds, party, targets)) {
        return {Status::Unsupported, Refusal::Strikes};
    }
    combat.m_attacks.updateShields(frame.seconds, party, targets);
    combat.m_attacks.updateArmour(frame.seconds, party, targets);
    combat.m_effects.update(frame.seconds);
    if (staged) {
        if (PartyPickups::touchesPickup(party, world, m_fixtures.chests())) {
            return {Status::Unsupported, Refusal::Pickups};
        }
        PartyPickups::cleanupCollectedChests(world, m_fixtures.chests());
        world.updateTriggers(frame.seconds, triggerVisitors(party, collision));
        m_fixtures.syncFloors();
        opponents.syncFloors();
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
    const auto after = m_checkpoint.checkpoint();
    const bool accepted = after && supported(*after, party, frame.seconds);
    timings.postconditionNanoseconds = elapsed(postconditionBegin);
    if (!accepted) {
        return {Status::Unsupported, Refusal::Postcondition};
    }
    result.status = Status::Advanced;
    undo.accept();
    return result;
}

} // namespace gdl::game
