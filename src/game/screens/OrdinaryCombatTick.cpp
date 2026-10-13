#include "game/screens/OrdinaryCombatTick.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

#include "game/players/PowerupEffects.h"
#include "game/players/Progression.h"
#include "game/screens/FloorRiding.h"
#include "game/screens/PartyRecords.h"
#include "game/world/CameraMovementLimit.h"

namespace gdl::game {
namespace {
bool finite(const Vec3& point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}
bool ordinaryAction(PlayerAnimator::Action action) {
    return PlayerAnimator::locomotion(action) || PlayerAnimator::rollbackReactionAction(action) ||
           action == PlayerAnimator::Action::DefendRaise ||
           action == PlayerAnimator::Action::Defend ||
           action == PlayerAnimator::Action::DefendLower;
}
bool ordinaryInput(const PlayInput& input) {
    return std::isfinite(input.move.direction.x) && std::isfinite(input.move.direction.y) &&
           std::abs(input.move.direction.x) <= 1 && std::abs(input.move.direction.y) <= 1 &&
           std::isfinite(input.move.magnitude) && input.move.magnitude >= 0 &&
           input.move.magnitude <= 1 && (!input.aimDirection || finite(*input.aimDirection)) &&
           !input.attack && !input.strongAttack && !input.combo && !input.chargePressed &&
           !input.attackPressed && !input.turboAttackPressed && !input.usePotion &&
           !input.throwPotion && !input.shieldPotion && !input.selector.up &&
           !input.selector.down && !input.selector.left && !input.selector.right;
}
bool quiet(const PlayerAttacksCheckpoint::State& attacks) {
    return attacks.items.empty() && attacks.potions.empty() && attacks.strikes.strikes.empty() &&
           attacks.strikeEffects.empty() && attacks.strikeSources.empty() &&
           attacks.attachments.empty() && attacks.particles.empty() && attacks.shields.empty();
}
bool quiet(const PlayerArsenalCheckpoint::State& arsenal) {
    const auto& missiles = arsenal.missiles;
    return arsenal.casts.empty() && missiles.missiles.empty() && missiles.impacts.empty() &&
           missiles.history.empty() && missiles.visuals.effects.empty() && missiles.ricochetIn == 0;
}
bool quiet(const OpponentState& opponents) {
    const auto& enemies = opponents.enemies;
    const auto& missiles = opponents.missiles;
    return enemies.losses.empty() && enemies.feedback.empty() && enemies.bursts.empty() &&
           enemies.deathEvents.empty() && enemies.deathShots.empty() && enemies.tagged.empty() &&
           missiles.hits.empty() && missiles.rockHits.empty() && missiles.pickupBlasts.empty() &&
           missiles.bursts.empty() && std::ranges::none_of(enemies.cues, [](const auto& cue) {
               return cue.kind == EnemyCue::Kind::Yell;
           });
}
bool standingFloor(const PlayerRuntime& player, const WorldCollision& collision) {
    const auto& actor = player.actor;
    const auto support = FloorRiding::support(actor, collision);
    return support && !collision.moving(support->object) &&
           !collision.floorExitBlocked(support->object) &&
           (support->objectFlags & (FloorRiding::kMoving | FloorRiding::kKeptApart)) == 0 &&
           std::abs(support->y - actor.position().y) <= 0.0001f && !player.floor.placement &&
           (player.floor.flags & (FloorRiding::kMoving | FloorRiding::kKeptApart)) == 0;
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
        &fixtures.weapons != &resources.weapons || &fixtures.device != &resources.device) {
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
    // These are the actual bound owners, not an empty obstacle list supplied by
    // the caller. Their updates may spend keys, produce drops, or mutate world
    // geometry, none of which belongs to this combat transaction yet.
    return m_fixtures.m_chests.size() == 0 && m_fixtures.m_gates.size() == 0 &&
           m_fixtures.m_traps.size() == 0 && m_fixtures.m_barrels.size() == 0 &&
           m_fixtures.m_safeRocks.size() == 0 && m_fixtures.m_scenery.size() == 0 &&
           m_fixtures.m_rubble.size() == 0 && m_fixtures.m_clouds.empty() &&
           m_fixtures.m_blasts.empty() && m_fixtures.m_doomedChests.empty() &&
           !world.hasWorldExplosions() && world.collision().loaded() &&
           world.collision().movingObjectCount() == 0 && world.hazards().harmful() == 0 &&
           world.fallingScenery().size() == 0 && world.walls().size() == 0 &&
           world.placedItems().size() == 0;
}

bool OrdinaryCombatTick::supported(const OrdinaryCombatCheckpoint::State& state,
                                   std::span<const PlayerRuntime> party) const {
    if (!quiet(state.combat.attacks) || !quiet(state.combat.arsenal) || !quiet(state.opponents) ||
        !state.adapter.lessons.empty() || !state.adapter.generatorRewards.empty() ||
        !state.adapter.rockHits.empty() || !state.adapter.cueEffects.empty() ||
        state.adapter.shrink != 1 || state.opponents.enemies.shrink != 1 ||
        state.opponents.missiles.shrink != 1 || !state.combat.effects.effects.empty()) {
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
                            [&](const auto& row) { return !loaded(row.spawn.kind); }) ||
        std::ranges::any_of(state.opponents.generators.generators,
                            [&](const auto& row) { return row.state > 0 && !loaded(row.kind); })) {
        return false;
    }
    for (const auto& player : party) {
        if (player.life != PlayerLife::Standing || player.departed || player.figure == nullptr ||
            player.actor.save().autoAttack || player.capture.active() ||
            player.transport.active() || player.combo.active() || player.itTicks != 0 ||
            player.deathHeld != -1 || player.gesture != PlayerDeed::None ||
            player.gagSeconds != 0 || player.figure->animator().entering() ||
            !ordinaryAction(player.figure->animator().action()) ||
            !PlayerAnimator::rollbackReactionDeed(player.reaction) ||
            player.figure->familiarTier() != 0 ||
            player.figure->companion().kind() != PowerupCompanion::Kind::None ||
            !standingFloor(player, collision)) {
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
        if (!move.pending.empty() || !move.all.empty() || move.owed != 0 || move.weaponHidden) {
            return false;
        }
    }
    return true;
}

OrdinaryCombatTick::Result OrdinaryCombatTick::step(std::span<PlayerRuntime> party,
                                                    std::span<const PlayInput> inputs,
                                                    const Frame& frame) {
    if (!frame.valid() || inputs.size() > 4 || !bound(party)) {
        return {};
    }
    if (frame.damage.bossEncounter || frame.damage.cutscene || !environment() ||
        std::ranges::any_of(inputs, [](const auto& input) { return !ordinaryInput(input); })) {
        return {Status::Unsupported};
    }
    auto before = m_checkpoint.checkpoint();
    if (!before) {
        return {};
    }
    if (!supported(*before, party)) {
        return {Status::Unsupported};
    }
    const auto initialViews = LevelOpponents::enemyViews(party);
    if (!OpponentSimulation::combatSupported(
            before->opponents,
            {.ticks = frame.ticks, .seconds = frame.seconds, .players = initialViews})) {
        return {Status::Unsupported};
    }
    auto rewind = m_checkpoint.prepareRewind();
    if (!rewind) {
        return {};
    }
    Undo undo{m_checkpoint, std::move(*rewind)};
    auto& combat = m_checkpoint.m_combat;
    auto& opponents = m_checkpoint.m_opponents;
    const auto& world = opponents.m_resources->world;
    const auto& collision = world.collision();
    Result result;
    bool refused = false;
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
                if (!ordinaryAction(party[index].figure->animator().action())) {
                    refused = true;
                    return;
                }
                combat.m_attacks.updateTurbo(index, ticks, seconds, party,
                                             [&](s32, usize) { refused = true; });
            },
        .limitMovement =
            [&](usize index, const Vec3& from, const Vec3& to) {
                return party.size() == 1
                           ? to
                           : CameraMovementLimit::constrain(
                                 from, to, frame.attention, frame.camera, frame.projection,
                                 party[index].actor.followPoint() - party[index].actor.position());
            },
        .resolveMovement =
            [&](usize index, const Vec3& from, const Vec3& to) {
                return opponents.resolveMovement(party[index].actor, from, to);
            }};
    result.subjects = PartyMotion::step(party, inputs, false, frame.camera.yaw, frame.ticks,
                                        frame.seconds, collision, events);
    if (refused || std::ranges::any_of(party, [&](const auto& player) {
            return !standingFloor(player, collision);
        })) {
        return {Status::Unsupported};
    }
    PartyRecords::advanceTime(party, frame.seconds);
    for (auto& player : party) {
        LevelFixtures::observePlayer(player, player.actor.position());
    }
    LevelFixtures::advanceHitSounds(party, frame.ticks);
    for (auto& player : party) {
        PlayerHealth::advanceSurfaceGap(player, frame.seconds);
    }
    result.heartbeat = PlayerHealth::heartbeat(party, frame.ticks, frame.damage.tower);
    opponents.watch(
        ViewVolume::of(frame.camera, frame.projection.horizontalFov, frame.projection.aspect),
        frame.attention);
    opponents.beginOrdinaryFrame(frame.seconds, party);
    opponents.countOrdinaryPlayers(party);
    const auto views = LevelOpponents::enemyViews(party);
    const auto* level = world.level();
    const OpponentSimulation::Frame opponentFrame{
        .ticks = frame.ticks,
        .seconds = frame.seconds,
        .players = views,
        .collision = &collision,
        .missileSpeed = level != nullptr ? level->tuning.enemyMissileSpeed : 1};
    if (m_checkpoint.m_pools.stageCombat(opponentFrame, m_checkpoint.m_missiles) !=
        OpponentSimulation::Result::Advanced) {
        return {Status::Unsupported};
    }
    result.enemyCues = opponents.m_enemies.takeCues();
    if (std::ranges::any_of(result.enemyCues,
                            [](const auto& cue) { return cue.kind == EnemyCue::Kind::Yell; })) {
        return {Status::Unsupported};
    }
    // Native levels precede the ordinary blow queue. A new level needs healing,
    // equipment rebinding and lesson owners; refuse rather than emulate those.
    for (const auto& player : party) {
        if (combat.m_levels.observe(player.actor.player(),
                                    experienceLevel(player.actor.save().experience()))) {
            return {Status::Unsupported};
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
        return {Status::Unsupported};
    }
    result.damage = std::move(blows.outputs);
    // Empty attack owners still advance native visual clocks, extinguish a
    // previously held glow, and retire armour contact cooldowns. Admission
    // proves there is no outgoing damage source before these phases run.
    const PlayerAttacks::Targets targets{
        .opponents = opponents, .fixtures = m_fixtures, .fixtureEvents = {}};
    combat.m_attacks.updateProjectiles(frame.seconds, party, targets);
    combat.m_attacks.updateStrikes(frame.seconds, party, targets);
    combat.m_attacks.updateShields(frame.seconds, party, targets);
    combat.m_attacks.updateArmour(frame.seconds, party, targets);
    combat.m_effects.update(frame.seconds);
    const auto after = m_checkpoint.checkpoint();
    if (!after || !supported(*after, party)) {
        return {Status::Unsupported};
    }
    result.status = Status::Advanced;
    undo.accept();
    return result;
}

} // namespace gdl::game
