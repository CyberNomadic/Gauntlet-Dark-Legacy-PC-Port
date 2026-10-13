#include <algorithm>
#include <cmath>
#include <optional>

#include "engine/core/Types.h"

#include "game/combat/DamageTypes.h"
#include "game/enemies/DeathRules.h"
#include "game/netplay/CombatSnapshot.h"
#include "game/players/Progression.h"
#include "game/screens/FloorRiding.h"
#include "game/screens/HelpMessages.h"
#include "game/screens/PartyCollision.h"
#include "game/screens/PlayScene.h"
#include "game/world/CameraMovementLimit.h"

namespace gdl::game {
namespace {
bool modifierSlotsFinite(const Inventory& inventory) {
    return std::ranges::none_of(inventory.powerups, [](const auto& slot) {
        const bool timed =
            slot.kind == powerup::kWeapon || slot.kind == powerup::kSpeed ||
            (slot.kind == powerup::kArmor && (slot.flags & powerup::kShields) != 0) ||
            (slot.kind == powerup::kSpecial && (slot.flags & powerup::kSpeedBoost) != 0);
        return slot.working() && timed &&
               (!std::isfinite(slot.strength) ||
                (slot.kind == powerup::kSpeed && !std::isfinite(slot.charge)));
    });
}

f32 fireShieldSeconds(const Inventory& inventory) {
    f32 seconds = 0;
    for (const auto& slot : inventory.powerups) {
        if (!slot.working() || slot.kind != powerup::kArmor ||
            (slot.flags & powerup::kFireShield) == 0) {
            continue;
        }
        if (slot.strength < 0) {
            return -1;
        }
        seconds = std::max(seconds, slot.strength);
    }
    return seconds;
}
} // namespace

bool PlayScene::movementPredictable() const {
    return m_open && m_movementUpdated && !m_leaving && !spawning() && !m_messages.active() &&
           !m_switchCutscene.active() && !m_sumnerVisit.active() && !m_gameOver.active() &&
           !m_welcome.cutting() && !m_promotion.active() && !relicCeremonyOn();
}

std::optional<TurboEffectState> PlayScene::turboEffectPreview(s32 seat) const {
    const auto player = std::ranges::find_if(m_players, [seat](const auto& member) {
        return member.actor.player() == seat && !member.departed;
    });
    if (player == m_players.end() || !player->figure) {
        return std::nullopt;
    }
    const auto* archive = player->figure->loadedEffects();
    if (!archive || !m_weapons.loaded()) {
        return std::nullopt;
    }
    TurboEffectState state;
    state.meleeDamage =
        m_attacks.meleeDamageOf(static_cast<usize>(player - m_players.begin()), m_players);
    state.classBank = m_effects.textureCheckpoint(*archive, m_effects.textureLenders());
    state.weaponBank = m_effects.textureCheckpoint(m_weapons, m_effects.textureLenders());
    return state.valid() ? std::optional{state} : std::nullopt;
}

std::optional<PlayerLocomotionState> PlayScene::locomotionPreview(s32 seat) {
    const auto player = std::ranges::find_if(m_players, [seat](const auto& member) {
        return member.actor.player() == seat && !member.departed;
    });
    if (player == m_players.end() || m_world == nullptr || !movementPredictable() ||
        player->figure == nullptr || player->life != PlayerLife::Standing ||
        player->capture.active() || player->transport.active() || player->combo.partner >= 0 ||
        player->deathHeld >= 0 || player->reaction != PlayerDeed::None ||
        player->gesture != PlayerDeed::None || player->gagSeconds > 0 ||
        player->figure->animator().entering() || player->floor.placement ||
        (player->floor.flags & (FloorRiding::kMoving | FloorRiding::kKeptApart)) != 0) {
        return std::nullopt;
    }
    const auto speed = predictionSpeed(seat);
    if (!speed) {
        return std::nullopt;
    }
    const auto& actor = player->actor;
    const auto impulse = player->knockback.checkpoint();
    if (impulse.force != Vec3{0} || impulse.damage != 0 || impulse.flags != 0 ||
        impulse.velocity != Vec3{0} || impulse.shove != Vec3{0} || impulse.shoved ||
        impulse.pushed || impulse.fastSeconds != 0) {
        return std::nullopt;
    }
    const auto floor = FloorRiding::support(actor, m_world->collision());
    if (!floor || m_world->collision().moving(floor->object) ||
        m_world->collision().floorExitBlocked(floor->object) ||
        std::abs(floor->y - actor.position().y) > 0.0001f) {
        return std::nullopt;
    }
    PlayerLocomotionState result;
    result.origin = actor.position();
    result.ticks = PlayerLocomotionState::kMaxTicks;
    result.radius = std::min(PlayerLocomotionState::kMaxRadius,
                             PlayerActor::kMoveLimit * *speed * static_cast<f32>(result.ticks) /
                                     static_cast<f32>(simulationTickRate()) +
                                 0.01f);
    const auto nearby = [&](const Vec3& position, f32 radius) {
        const Vec3 offset = position - result.origin;
        return std::hypot(offset.x, offset.z) <= result.radius + actor.radius() + radius;
    };
    const auto appendBodies = [&](const auto& bodies) {
        for (const auto& body : bodies) {
            if (body.radius > 0 && body.height > 0 && nearby(body.base, body.radius)) {
                result.bodies.push_back({body.base, body.radius, body.height});
            }
        }
    };
    const auto worn = PowerupEffects::of(actor.save().progress().inventory);
    auto enemies = m_opponents.enemies().targets();
    if ((worn.armor & DeathRules::kProtection) == 0) {
        std::erase_if(enemies, [&](const auto& body) {
            return m_opponents.enemies().kindOf(body.id) == kDeathKind;
        });
    }
    appendBodies(enemies);
    appendBodies(m_opponents.critters().targets(true));
    appendBodies(m_opponents.bosses().targets());
    const auto appendBoxes = [&](std::vector<Obstacle>& output, const auto& boxes) {
        for (const auto& box : boxes) {
            const f32 extent =
                std::max(box.cylinderRadius, std::hypot(box.halfAcross, box.halfAlong));
            if (box.solid && nearby(box.centre, extent)) {
                output.push_back(box);
            }
        }
    };
    appendBoxes(result.generators, m_opponents.generators().obstacles());
    appendBoxes(result.fixtures, m_fixtures.obstacles());
    for (const auto& other : m_players) {
        if (&other != &*player && other.life == PlayerLife::Standing && !other.departed &&
            !other.capture.held() && !other.combo.riding &&
            nearby(other.actor.position(), other.actor.radius())) {
            result.party.push_back(
                {other.actor.position(), other.actor.radius(), other.actor.height()});
        }
    }
    // A movement input may request native automatic melee before applying pace.
    // Contact shields suppress that substitution in PlayerAttacks itself.
    result.automatic = m_context.config != nullptr && m_context.config->combat.autoMelee &&
                       actor.save().autoAttack &&
                       (worn.armor & (powerup::kFireShield | powerup::kLightningShield)) == 0;
    if (result.automatic) {
        const auto selection = m_attacks.meleeAcquisition(actor, attackTargets());
        auto geometry = m_attacks.meleeGeometry(actor, attackTargets(), result.radius, false);
        if (selection && geometry && selection->targets.size() <= PlayerMeleeState::kMaxTargets &&
            geometry->size() <= PlayerMeleeState::kMaxTargets) {
            PlayerMeleeState automatic;
            automatic.selection = *selection;
            automatic.captureYaw = std::remainder(actor.yaw(), kTwoPi);
            automatic.moving = PlayerMeleeState::Moving{
                result.origin, result.radius, result.ticks, std::move(*geometry), {}};
            if (automatic.valid()) {
                result.automaticGeometry = std::move(automatic);
            }
        }
    }
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}

std::optional<PlayerPotionState>
PlayScene::potionPreview(s32 seat, const PlayerLocomotionState& contacts) const {
    const auto player = std::ranges::find_if(m_players, [seat](const auto& member) {
        return member.actor.player() == seat && !member.departed;
    });
    if (player == m_players.end() || m_world == nullptr || !movementPredictable() ||
        simulationTickRate() != 60 || player->figure == nullptr ||
        player->life != PlayerLife::Standing || player->capture.active() ||
        player->transport.active() || player->combo.partner >= 0 || player->deathHeld >= 0 ||
        player->reaction != PlayerDeed::None || player->gesture != PlayerDeed::None ||
        player->gagSeconds > 0 || player->floor.object < 0 || player->floor.placement ||
        (player->floor.flags & (FloorRiding::kMoving | FloorRiding::kKeptApart)) != 0 ||
        !contacts.valid() || contacts.origin != player->actor.position() ||
        !rangedItemsPredictable(seat)) {
        return std::nullopt;
    }
    const auto& animator = player->figure->animator();
    if (!PlayerPotionState::supports(animator.checkpoint()) || player->knockback.pending() ||
        player->knockback.sliding() || player->knockback.pushed()) {
        return std::nullopt;
    }
    const auto& actor = player->actor;
    auto inventory = actor.save().progress().inventory;
    if ((inventory.potions.empty() && !animator.conjuring()) ||
        std::ranges::any_of(inventory.potions, [](s32 kind) { return kind < 1 || kind > 4; }) ||
        std::ranges::any_of(inventory.powerups, [](const auto& slot) {
            return slot.working() && (!std::isfinite(slot.strength) || !std::isfinite(slot.charge));
        })) {
        return std::nullopt;
    }
    PlayerPotionState result;
    result.potions = inventory.potions;
    for (usize kind = 0; kind < result.powers.size(); ++kind) {
        result.powers[kind] = m_arsenal.potionPowerOf(actor, static_cast<s32>(kind));
        result.bonuses[kind] = damage::colourBonus(actor.save().color, static_cast<u32>(kind));
    }
    result.damageFlags = damage::magicHeal(experienceLevel(actor.save().experience()));
    result.textureFrame = m_arsenal.missiles().visuals().textureFrame(m_weapons);
    const auto worn = PowerupEffects::of(inventory);
    if (!std::isfinite(worn.magicAdd)) {
        return std::nullopt;
    }
    // Inventory advances before PartyMotion. Stop before any native tuning/pace
    // change, including one of several Magic or Speed slots expiring. A boss may
    // awaken during the prefix, so its fastest clock bounds the guarantee.
    const bool boss = m_world->level() != nullptr && m_world->level()->bossType >= 0;
    const f32 seconds = m_world->isTower() ? 0 : (boss ? 3.0f : 1.0f) / 60;
    const u32 horizon = std::min(contacts.ticks, PlayerPotionState::kMaxTicks);
    for (; result.ticks < horizon; ++result.ticks) {
        inventory.advance(seconds);
        const auto next = PowerupEffects::of(inventory);
        if (next.weapon != worn.weapon || next.armor != worn.armor ||
            next.special != worn.special || next.paceAdd != worn.paceAdd ||
            next.magicAdd != worn.magicAdd) {
            break;
        }
    }
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}

bool PlayScene::platformPredictable(s32 seat, const WorldMotion::State& motion, u32 ticks) const {
    const auto player = std::ranges::find_if(m_players, [seat](const auto& member) {
        return member.actor.player() == seat && !member.departed;
    });
    // Most seats are on still ground. Do not copy a level or advance any clocks
    // until the actual native floor proves this is a moving rider.
    if (player == m_players.end() || m_world == nullptr || !movementPredictable() || ticks == 0 ||
        ticks > PlatformState::kMaxTicks || player->figure == nullptr ||
        player->life != PlayerLife::Standing || !player->floor.placement ||
        (player->floor.flags & FloorRiding::kMoving) == 0 ||
        !m_world->collision().moving(player->floor.object)) {
        return false;
    }
    const auto& actor = player->actor;
    const auto& animator = player->figure->animator();
    const auto impulse = player->knockback.checkpoint();
    const auto worn = PowerupEffects::of(actor.save().progress().inventory);
    constexpr u32 kArmor = powerup::kInvulnerable | powerup::kGoldInvulnerable | powerup::kShields;
    const auto speed = predictionSpeed(seat);
    const auto modifiers = weaponModifiers(seat);
    if (!PlayerAnimator::locomotion(animator.action()) || animator.entering() ||
        player->capture.active() || player->transport.active() || player->combo.partner >= 0 ||
        player->deathHeld >= 0 || player->reaction != PlayerDeed::None ||
        player->gesture != PlayerDeed::None || player->gagSeconds > 0 || impulse.force != Vec3{0} ||
        impulse.damage != 0 || impulse.flags != 0 || impulse.velocity != Vec3{0} ||
        impulse.shove != Vec3{0} || impulse.shoved || impulse.pushed || impulse.fastSeconds != 0 ||
        !speed || !PlayerMissiles::replayModifiers(worn.weapon) || (worn.armor & ~kArmor) != 0 ||
        (worn.special & ~powerup::kSpeedBoost) != 0 || (modifiers && modifiers->ticks < ticks) ||
        (m_context.config != nullptr && m_context.config->combat.autoMelee &&
         actor.save().autoAttack)) {
        return false;
    }
    const auto displayed = viewCamera();
    const auto& nativeCamera = bossCameraOn() ? m_bossCamera.camera() : m_camera.camera();
    if (displayed.position != nativeCamera.position || displayed.pitch != nativeCamera.pitch ||
        displayed.yaw != nativeCamera.yaw || displayed.roll != nativeCamera.roll) {
        return false;
    }
    const auto& objects = m_world->layout().objects();
    const auto validObject = [&](s32 object) {
        return object >= 0 && static_cast<usize>(object) < objects.size();
    };
    if (!validObject(player->floor.object)) {
        return false;
    }
    s32 root = player->floor.object;
    s32 ancestor = root;
    for (usize guard = 0; validObject(ancestor) && guard < objects.size(); ++guard) {
        const s32 object = ancestor;
        if (m_world->scene().moving(static_cast<usize>(object))) {
            root = object;
        }
        ancestor = objects[static_cast<usize>(object)].parent;
    }
    const auto below = [&](s32 object) {
        for (usize guard = 0; validObject(object) && guard < objects.size(); ++guard) {
            if (object == root) {
                return true;
            }
            object = objects[static_cast<usize>(object)].parent;
        }
        return false;
    };
    // WorldDestruction and hazard callbacks are deliberately not replayed. A
    // cart's cycle can change solidity independently of its authored transform.
    for (usize object = 0; object < objects.size(); ++object) {
        if (below(static_cast<s32>(object)) &&
            (m_world->hazards().flagsOf(static_cast<s32>(object)) & HazardSurfaces::kHarmMask) !=
                0) {
            return false;
        }
    }
    auto scene = m_world->scene();
    auto collision = m_world->collision();
    WorldMotion continuation;
    continuation.bind(m_world->layout());
    if (!continuation.prepare(motion, scene.geometry(), scene, collision)) {
        return false;
    }
    std::array<PlayerRuntime, 1> rider;
    rider[0].actor = actor;
    rider[0].actor.setPaceBonus(worn.paceAdd);
    rider[0].floor = player->floor;
    const f32 seconds = 1.0f / static_cast<f32>(simulationTickRate());
    f32 carrierDistance = 0;
    bool moved = false;
    bool locked = collision.floorExitBlocked(player->floor.object);
    const auto initial = scene.worldTransform(static_cast<usize>(player->floor.object));
    Mat4 previous = *player->floor.placement;
    const auto rigid = [](const Mat4& transform) {
        const Mat3 basis{transform};
        const Mat3 lengths = glm::transpose(basis) * basis;
        for (s32 column = 0; column < 3; ++column) {
            for (s32 row = 0; row < 3; ++row) {
                if (!std::isfinite(lengths[column][row]) ||
                    std::abs(lengths[column][row] - (column == row ? 1.0f : 0.0f)) > 0.001f) {
                    return false;
                }
            }
        }
        return true;
    };
    for (u32 tick = 0; tick < ticks; ++tick) {
        if (!continuation.beforeMotion(seconds, scene, collision)) {
            return false;
        }
        const auto now = collision.objectTransform(player->floor.object);
        if (!now || !rigid(*now * glm::inverse(previous))) {
            return false;
        }
        previous = *now;
        moved = moved || *now != initial;
        FloorRiding::carry(rider[0], collision);
        const Vec3 carried = rider[0].actor.position();
        rider[0].actor.update({}, 0, seconds, &collision);
        FloorRiding::land(rider, 0, carried, collision);
        const auto support = FloorRiding::support(rider[0].actor, collision);
        if (!support || support->object != player->floor.object ||
            rider[0].floor.object != player->floor.object ||
            std::abs(support->y - carried.y) > 0.0001f ||
            glm::distance(carried, rider[0].actor.position()) > 0.0001f) {
            return false;
        }
        carrierDistance = std::max(carrierDistance, glm::distance(carried, actor.position()));
        if (!continuation.afterMotion(seconds, scene, collision)) {
            return false;
        }
        moved = moved || scene.worldTransform(static_cast<usize>(player->floor.object)) != initial;
        locked = locked && collision.floorExitBlocked(player->floor.object);
    }
    if (!moved || !std::isfinite(carrierDistance)) {
        return false;
    }
    // Every possible local walking direction is enclosed, including carrier
    // rotation/translation before the player gets to choose the next direction.
    const f32 reach = carrierDistance + *speed * seconds * static_cast<f32>(ticks) + actor.reach();
    const auto nearby = [&](const Vec3& position, f32 radius) {
        const Vec3 offset = position - actor.position();
        return std::hypot(offset.x, offset.z) <= reach + radius;
    };
    const auto boxNearby = [&](const Obstacle& box) {
        return nearby(box.centre,
                      std::max(box.cylinderRadius, std::hypot(box.halfAcross, box.halfAlong)));
    };
    for (const auto& other : m_players) {
        if (&other != &*player && !other.departed &&
            (below(other.floor.object) || nearby(other.actor.position(), other.actor.radius()) ||
             (other.floor.flags & FloorRiding::kKeptApart) != 0)) {
            return false;
        }
    }
    const auto& enemies = m_opponents.enemies();
    for (s32 id = 0; id < Enemies::kMost; ++id) {
        if (enemies.observe(id) &&
            (below(enemies.floorOf(id)) || nearby(enemies.positionOf(id), enemies.radiusOf(id)))) {
            return false;
        }
    }
    for (const auto& fighter : m_opponents.critters().fighters()) {
        if (fighter.present() && (below(fighter.floorObject()) ||
                                  nearby(fighter.position(), fighter.radius() * fighter.scale()))) {
            return false;
        }
    }
    for (const auto& target : m_opponents.critters().targets(true)) {
        if (nearby(target.base, target.radius)) {
            return false;
        }
    }
    if (m_opponents.bosses().present()) {
        const auto& boss = m_opponents.bosses().fighter();
        if (below(boss.floorObject()) || nearby(boss.position(), boss.radius() * boss.scale())) {
            return false;
        }
    }
    for (usize index = 0; index < m_opponents.generators().count(); ++index) {
        if (below(m_opponents.generators().floorOf(static_cast<s32>(index)))) {
            return false;
        }
    }
    if (std::ranges::any_of(m_opponents.generators().obstacles(), boxNearby) ||
        std::ranges::any_of(m_opponents.statues().obstacles(), boxNearby) ||
        std::ranges::any_of(m_fixtures.obstacles(), boxNearby)) {
        return false;
    }
    for (usize index = 0; index < m_world->placedItems().size(); ++index) {
        const auto& item = m_world->placedItems().item(index);
        if (!item.taken && ((item.floor && below(item.floor->object)) ||
                            (item.visible && nearby(item.position, item.radius)))) {
            return false;
        }
    }
    for (usize index = 0; index < m_fixtures.chests().size(); ++index) {
        const auto& chest = m_fixtures.chests().chest(index);
        if (!chest.gone && chest.shown &&
            ((chest.floor && below(chest.floor->object)) || boxNearby(chest.box))) {
            return false;
        }
    }
    const auto attached = [&](const auto& item) {
        return item.shown && (below(item.support.object()) || boxNearby(item.box));
    };
    for (usize index = 0; index < m_fixtures.barrels().size(); ++index) {
        if (attached(m_fixtures.barrels().barrel(index))) {
            return false;
        }
    }
    for (usize index = 0; index < m_fixtures.gates().size(); ++index) {
        if (attached(m_fixtures.gates().gate(index))) {
            return false;
        }
    }
    for (usize index = 0; index < m_fixtures.traps().size(); ++index) {
        if (attached(m_fixtures.traps().trap(index))) {
            return false;
        }
    }
    for (usize index = 0; index < m_fixtures.scenery().size(); ++index) {
        if (attached(m_fixtures.scenery().prop(index))) {
            return false;
        }
    }
    for (usize index = 0; index < m_world->walls().size(); ++index) {
        if (m_world->walls().standing(index)) {
            const auto target = m_world->walls().target(index, 0);
            if (below(m_world->walls().wall(index).object) || nearby(target.base, target.radius)) {
                return false;
            }
        }
    }
    for (usize index = 0; index < m_world->fallingScenery().size(); ++index) {
        const auto& piece = m_world->fallingScenery().piece(index);
        if (piece.shown && nearby(piece.motion.position + piece.centre, piece.radius)) {
            return false;
        }
    }
    for (usize index = 0; index < m_portals.size(); ++index) {
        const auto& portal = m_portals.portal(index);
        if (!portal.consumed && (below(portal.support) || nearby(portal.position, portal.radius))) {
            return false;
        }
    }
    for (usize index = 0; index < m_transporters.size(); ++index) {
        const auto& pad = m_transporters.pad(index);
        if (nearby(pad.position, pad.radius)) {
            return false;
        }
    }
    const auto& triggers = m_world->triggers();
    for (usize index = 0; index < triggers.size(); ++index) {
        const auto& trigger = triggers.trigger(index);
        const bool affected = below(trigger.target);
        if (affected && (trigger.kind & LevelTrigger::kFades) != 0) {
            return false;
        }
        const auto cursor = std::ranges::find(motion.triggers.targets, trigger.target,
                                              &LevelTriggers::Motion::Target::object);
        const bool permanent =
            trigger.forced || (cursor != motion.triggers.targets.end() && cursor->forced) ||
            ((trigger.kind & LevelTrigger::kFades) == 0 &&
             (trigger.kind & (LevelTrigger::kOpensOnce | LevelTrigger::kCloses |
                              LevelTrigger::kToggles)) == LevelTrigger::kOpensOnce &&
             cursor != motion.triggers.targets.end() && cursor->open);
        const bool retained =
            locked && trigger.heldContacts != 0 &&
            (trigger.flags & (LevelTrigger::kKeepContact | LevelTrigger::kOnTarget)) ==
                (LevelTrigger::kKeepContact | LevelTrigger::kOnTarget) &&
            (trigger.target == player->floor.object ||
             trigger.target == objects[static_cast<usize>(player->floor.object)].parent);
        if ((below(trigger.floor) && triggers.figure(index) != nullptr) ||
            (trigger.enabled && !permanent && !retained &&
             (affected || nearby(trigger.spot, trigger.radius * LevelTriggers::kMetReach)))) {
            return false;
        }
    }
    for (usize index = 0; index < m_world->rotators().size(); ++index) {
        const auto& rotator = m_world->rotators().rotator(index);
        if (rotator.subtype == Rotators::kTurnedByPad && !rotator.started &&
            nearby(rotator.spot, rotator.radius)) {
            return false;
        }
    }
    return true;
}

bool PlayScene::itemsPredictable(s32 seat) const {
    const auto player = std::ranges::find_if(m_players, [seat](const auto& member) {
        return member.actor.player() == seat && !member.departed;
    });
    return player != m_players.end() && rangedItemsPredictable(seat) &&
           (PowerupEffects::of(player->actor.save().progress().inventory).weapon &
            ~PlayerMissiles::kPlainModifiers) == 0;
}

bool PlayScene::rangedItemsPredictable(s32 seat) const {
    const auto player = std::ranges::find_if(m_players, [seat](const auto& member) {
        return member.actor.player() == seat && !member.departed;
    });
    if (player == m_players.end()) {
        return false;
    }
    const auto& inventory = player->actor.save().progress().inventory;
    const auto worn = PowerupEffects::of(inventory);
    constexpr u32 kPassiveArmor = powerup::kInvulnerable | powerup::kGoldInvulnerable;
    return PlayerMissiles::replayModifiers(worn.weapon) && (worn.armor & ~kPassiveArmor) == 0 &&
           (worn.special & ~powerup::kSpeedBoost) == 0 && std::isfinite(worn.paceAdd) &&
           modifierSlotsFinite(inventory);
}

std::optional<f32> PlayScene::predictionSpeed(s32 seat) const {
    const auto player = std::ranges::find_if(m_players, [seat](const auto& member) {
        return member.actor.player() == seat && !member.departed;
    });
    if (player == m_players.end()) {
        return std::nullopt;
    }
    const auto& inventory = player->actor.save().progress().inventory;
    const auto worn = PowerupEffects::of(inventory);
    if (!std::isfinite(worn.paceAdd) || !modifierSlotsFinite(inventory) ||
        ((worn.special & powerup::kSpeedBoost) != 0 &&
         !PlayerMissiles::replayModifiers(worn.weapon))) {
        return std::nullopt;
    }
    if ((worn.armor & powerup::kShields) != 0) {
        constexpr u32 kArmor =
            powerup::kInvulnerable | powerup::kGoldInvulnerable | powerup::kShields;
        // Reflect alone permits native automatic melee while walking. Fire and
        // Lightning suppress that substitution in PlayerAttacks; otherwise its
        // potentially moving targets must remain authoritative.
        const bool automatic = m_context.config != nullptr && m_context.config->combat.autoMelee &&
                               player->actor.save().autoAttack;
        const bool contactShield =
            (worn.armor & (powerup::kFireShield | powerup::kLightningShield)) != 0;
        if ((worn.armor & ~kArmor) != 0 || (worn.special & ~powerup::kSpeedBoost) != 0 ||
            !PlayerMissiles::replayModifiers(worn.weapon) || (automatic && !contactShield)) {
            return std::nullopt;
        }
    }
    // Pickups run after PartyMotion. Its cached pace can still belong to the
    // preceding inventory until the next native movement step refreshes it.
    auto actor = player->actor;
    actor.setPaceBonus(worn.paceAdd);
    const f32 speed = actor.speed();
    return std::isfinite(speed) ? std::optional{speed} : std::nullopt;
}

bool PlayScene::rangedPredictable(s32 seat) {
    const auto player = std::ranges::find_if(m_players, [seat](const auto& member) {
        return member.actor.player() == seat && !member.departed;
    });
    if (player == m_players.end() || !movementPredictable() || player->figure == nullptr ||
        !rangedItemsPredictable(seat)) {
        return false;
    }
    // Mouse aiming can turn immediately; cardinal probes miss the diagonal gaps
    // in the native acquisition cone. Use its all-bearing bounds instead.
    return m_attacks.rangedOnly(player->actor, attackTargets());
}

std::optional<PlayerWeaponModifiers> PlayScene::weaponModifiers(s32 seat) const {
    const auto player = std::ranges::find_if(m_players, [seat](const auto& member) {
        return member.actor.player() == seat && !member.departed;
    });
    if (player == m_players.end() || m_world == nullptr) {
        return std::nullopt;
    }
    auto inventory = player->actor.save().progress().inventory;
    const auto worn = PowerupEffects::of(inventory);
    const u32 flags = worn.weapon;
    const bool speed = (worn.special & powerup::kSpeedBoost) != 0;
    const u32 shields = worn.armor & powerup::kShields;
    if ((flags == 0 && !speed && shields == 0) || !PlayerMissiles::replayModifiers(flags) ||
        !std::isfinite(worn.paceAdd) || !modifierSlotsFinite(inventory)) {
        return std::nullopt;
    }
    PlayerWeaponModifiers modifiers{flags, 0, speed, shields};
    // The boss may awaken before the next checkpoint. Certify against its
    // fastest native clock even while it sleeps; pauses can only extend items.
    const bool boss = m_world->level() != nullptr && m_world->level()->bossType >= 0;
    f32 rate = 1;
    if (m_world->isTower()) {
        rate = 0;
    } else if (boss) {
        rate = 3;
    }
    const f32 seconds = (1.0f / static_cast<f32>(simulationTickRate())) * rate;
    if ((shields & powerup::kFireShield) != 0) {
        modifiers.fireSeconds = fireShieldSeconds(inventory);
        modifiers.inventoryStep = seconds;
        if (boss) {
            const auto view = m_opponents.bosses().view();
            if (!view.awake || !view.alive) {
                modifiers.inventoryStep = 0;
            }
        }
    }
    for (u32 tick = 0; tick < PlayerWeaponModifiers::kMaxTicks; ++tick) {
        inventory.advance(seconds);
        const auto next = PowerupEffects::of(inventory);
        if (next.weapon != flags || ((next.special & powerup::kSpeedBoost) != 0) != speed ||
            next.paceAdd != worn.paceAdd || (next.armor & powerup::kShields) != shields) {
            break;
        }
        ++modifiers.ticks;
    }
    return modifiers;
}

std::optional<PlayerMeleeState> PlayScene::meleePreview(s32 seat) {
    return attackPreview(seat, true);
}

std::optional<PlayerMeleeState> PlayScene::turboGeometry(s32 seat) {
    return attackPreview(seat, false);
}

std::optional<PlayerTurboState> PlayScene::turboPreview(s32 seat) const {
    const auto player = std::ranges::find_if(m_players, [seat](const auto& member) {
        return member.actor.player() == seat && !member.departed;
    });
    if (player == m_players.end() || player->figure == nullptr) {
        return std::nullopt;
    }
    const auto action = player->figure->animator().action();
    if (!PlayerAnimator::rollbackAction(action) && !PlayerAnimator::rollbackMeleeAction(action) &&
        !PlayerAnimator::rollbackMovingMeleeAction(action) &&
        !PlayerAnimator::rollbackTurboAction(action) &&
        !PlayerAnimator::rollbackShoveAction(action)) {
        return std::nullopt;
    }
    const auto* stats = m_classes.stats(player->actor.save().character);
    PlayerTurboState result{player->turbo.checkpoint(), player->move.checkpoint()};
    TurboMove timeline;
    if (stats == nullptr || !result.valid() || !timeline.restore(result.move, stats, action)) {
        return std::nullopt;
    }
    return result;
}

bool PlayScene::shovePredictable(s32 seat, const PlayerMeleeState& geometry) const {
    const auto player = std::ranges::find_if(m_players, [seat](const auto& member) {
        return member.actor.player() == seat && !member.departed;
    });
    if (player == m_players.end() || m_world == nullptr || !movementPredictable() ||
        !geometry.valid() || !geometry.moving ||
        geometry.moving->origin != player->actor.position()) {
        return false;
    }
    const auto& actor = player->actor;
    const f32 reach = actor.radius() + PlayerAttacks::kRamReach + geometry.moving->radius;
    const auto& walls = m_world->walls();
    for (usize index = 0; index < walls.size(); ++index) {
        if (walls.standing(index) && walls.target(index, 0).touches(actor.followPoint(), reach)) {
            return false;
        }
    }
    const auto nearby = [&](const Obstacle& box) {
        const f32 extent = std::max(box.cylinderRadius, std::hypot(box.halfAcross, box.halfAlong));
        const Vec3 offset = box.centre - actor.position();
        // Ignore vertical separation here: climbing inside the region can enter
        // a fixture's height band. Native ram contact ignores its solid flag too.
        return std::hypot(offset.x, offset.z) <= reach + extent;
    };
    const auto& barrels = m_fixtures.barrels();
    for (usize index = 0; index < barrels.size(); ++index) {
        if (barrels.standing(index) && nearby(barrels.barrel(index).box)) {
            return false;
        }
    }
    const auto& rocks = m_fixtures.safeRocks();
    for (usize index = 0; index < rocks.size(); ++index) {
        if (rocks.standing(index) && nearby(rocks.rock(index).obstacle)) {
            return false;
        }
    }
    return true;
}

std::optional<PlayerMeleeState> PlayScene::attackPreview(s32 seat, bool requireClose) {
    const auto player = std::ranges::find_if(m_players, [seat](const auto& member) {
        return member.actor.player() == seat && !member.departed;
    });
    if (player == m_players.end() || !movementPredictable() || player->figure == nullptr ||
        m_world == nullptr || player->life != PlayerLife::Standing ||
        (m_context.config != nullptr && m_context.config->combat.autoMelee)) {
        return std::nullopt;
    }
    const auto speed = predictionSpeed(seat);
    if (!itemsPredictable(seat) || !speed) {
        return std::nullopt;
    }
    const auto& actor = player->actor;
    const auto floor = m_world->collision().floorAt(actor.position(), 0.25f, 0.25f);
    if (!floor || m_world->collision().moving(floor->object) ||
        std::abs(floor->y - actor.position().y) > 0.01f) {
        return std::nullopt;
    }
    auto selection = m_attacks.meleeAcquisition(actor, attackTargets());
    if (!selection || selection->targets.size() > PlayerMeleeState::kMaxTargets) {
        return std::nullopt;
    }
    PlayerMeleeState result;
    result.selection = std::move(*selection);
    result.captureYaw = std::remainder(actor.yaw(), kTwoPi);
    auto moving = [&]() -> std::optional<PlayerMeleeState::Moving> {
        const auto& animator = player->figure->animator();
        const auto impulse = player->knockback.checkpoint();
        const auto support = FloorRiding::support(actor, m_world->collision());
        const auto displayedCamera = viewCamera();
        const auto& movementCamera = bossCameraOn() ? m_bossCamera.camera() : m_camera.camera();
        if (player->capture.active() || player->transport.active() || player->combo.partner >= 0 ||
            player->deathHeld >= 0 || player->reaction != PlayerDeed::None ||
            player->gesture != PlayerDeed::None || player->gagSeconds > 0 || animator.entering() ||
            impulse.force != Vec3{0} || impulse.damage != 0 || impulse.flags != 0 ||
            impulse.velocity != Vec3{0} || impulse.shove != Vec3{0} || impulse.shoved ||
            impulse.pushed || impulse.fastSeconds != 0 || player->floor.placement ||
            (player->floor.flags & FloorRiding::kMoving) != 0 || !support ||
            m_world->collision().moving(support->object) ||
            (support->objectFlags & FloorRiding::kMoving) != 0 ||
            m_world->collision().floorExitBlocked(support->object) ||
            std::abs(support->y - actor.position().y) > 0.0001f ||
            m_world->collision().floorExitBlocked(player->floor.object) ||
            m_world->collision().floorExitBlocked(floor->object) ||
            std::abs(floor->y - actor.position().y) > 0.0001f ||
            displayedCamera.position != movementCamera.position ||
            displayedCamera.pitch != movementCamera.pitch ||
            displayedCamera.yaw != movementCamera.yaw ||
            displayedCamera.roll != movementCamera.roll) {
            return std::nullopt;
        }
        PlayerMeleeState::Moving region;
        region.origin = actor.position();
        region.ticks = PlayerMeleeState::kMaxTicks;
        // Native attack/run paces can exceed walking speed. Capture a superset
        // of that travel; local replay still enforces the exact action pace.
        region.radius =
            std::min(PlayerMeleeState::kMaxRadius, 1.5f * *speed * static_cast<f32>(region.ticks) /
                                                           static_cast<f32>(simulationTickRate()) +
                                                       0.01f);
        for (const auto& other : m_players) {
            if (&other == &*player || other.departed || other.life != PlayerLife::Standing) {
                continue;
            }
            const auto offset = other.actor.position() - region.origin;
            if (std::hypot(offset.x, offset.z) <=
                region.radius + actor.radius() + other.actor.radius()) {
                // Party contact can shove either participant. Do not replay one
                // player's half of that transaction in isolation.
                return std::nullopt;
            }
        }
        auto geometry =
            m_attacks.meleeGeometry(actor, attackTargets(), region.radius, requireClose);
        if (!geometry || geometry->size() > PlayerMeleeState::kMaxTargets) {
            return std::nullopt;
        }
        region.targets = std::move(*geometry);
        const auto append = [&](const auto& boxes) {
            for (const auto& box : boxes) {
                const auto offset = box.centre - region.origin;
                const f32 extent =
                    std::max(box.cylinderRadius, std::hypot(box.halfAcross, box.halfAlong));
                if (box.solid &&
                    std::hypot(offset.x, offset.z) <= region.radius + actor.radius() + extent) {
                    region.obstacles.push_back(box);
                }
            }
        };
        append(m_opponents.generators().obstacles());
        append(m_fixtures.obstacles());
        append(m_opponents.statues().obstacles());
        if (region.obstacles.size() > PlayerMeleeState::kMaxObstacles) {
            return std::nullopt;
        }
        return region;
    }();
    if (moving) {
        result.moving = std::move(moving);
        if (result.valid()) {
            return result;
        }
    }
    if (!requireClose) {
        // Turbo movement must never fall back to stationary acquisition alone.
        return std::nullopt;
    }
    const auto& attackStep = player->attackStep;
    if (actor.moving() ||
        (attackStep && glm::distance(attackStep->from, attackStep->to) > 0.0001f)) {
        return std::nullopt;
    }
    result.moving.reset();
    result.assistedAim = m_attacks.aim(actor, actor.facing(), attackTargets());
    if (result.assistedAim) {
        // A stationary controller attack turns to this point before advancing the
        // animation. Only keep a context whose next identical query stays there.
        auto turned = actor;
        turned.faceToward(*result.assistedAim);
        const auto next = m_attacks.aim(turned, turned.facing(), attackTargets());
        if (!next || glm::distance(*next, *result.assistedAim) > 0.0001f) {
            return std::nullopt;
        }
    }
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}

std::optional<PlayerMissiles::WeaponThrow> PlayScene::weaponPreview(s32 seat) {
    const auto player = std::ranges::find_if(m_players, [seat](const auto& member) {
        return member.actor.player() == seat && !member.departed;
    });
    if (player == m_players.end() || player->figure == nullptr) {
        return std::nullopt;
    }
    if (!rangedItemsPredictable(seat)) {
        return std::nullopt;
    }
    auto weapon = m_arsenal.weaponThrow(player->actor, player->figure->animator().attackSeconds());
    if (!weapon || weapon->multiplayer != MultiplayerMode::Normal ||
        (weapon->shots != 1 && weapon->shots != 3 && weapon->shots != 5)) {
        return std::nullopt;
    }
    weapon->target = m_attacks.aim(player->actor, player->actor.facing(), attackTargets());
    return weapon;
}

u32 PlayScene::knockbackPreview(s32 seat) const {
    const auto player = std::ranges::find_if(m_players, [seat](const auto& member) {
        return member.actor.player() == seat && !member.departed;
    });
    if (player == m_players.end() || !movementPredictable() || player->figure == nullptr ||
        player->life != PlayerLife::Standing || player->capture.active() ||
        player->transport.active() || player->combo.partner >= 0 || player->deathHeld >= 0 ||
        player->gesture != PlayerDeed::None || player->gagSeconds > 0 ||
        (m_context.config != nullptr && m_context.config->combat.autoMelee)) {
        return 0;
    }
    const auto& animator = player->figure->animator();
    // The packet's displayed camera includes shakes; native movement uses the
    // unshaken camera. Do not certify a corridor with different projection bounds.
    const auto displayedCamera = viewCamera();
    const auto& movementCamera = bossCameraOn() ? m_bossCamera.camera() : m_camera.camera();
    if (displayedCamera.position != movementCamera.position ||
        displayedCamera.pitch != movementCamera.pitch ||
        displayedCamera.yaw != movementCamera.yaw || displayedCamera.roll != movementCamera.roll ||
        player->floor.placement || (player->floor.flags & FloorRiding::kMoving) != 0 ||
        m_world->collision().floorExitBlocked(player->floor.object)) {
        return 0;
    }
    const auto impulse = player->knockback.checkpoint();
    const bool reacting = PlayerAnimator::rollbackReactionAction(animator.action());
    const auto worn = PowerupEffects::of(player->actor.save().progress().inventory);
    if ((!reacting && !PlayerAnimator::locomotion(animator.action())) ||
        !PlayerAnimator::rollbackReactionDeed(player->reaction) || animator.entering() ||
        (!reacting && player->reaction == PlayerDeed::None && !player->knockback.pending() &&
         !player->knockback.sliding()) ||
        (impulse.flags & ~Knockback::kKnockBack) != 0 || impulse.shove != Vec3{0} ||
        impulse.shoved || impulse.pushed || worn.weapon != 0 || worn.armor != 0 ||
        (worn.special & ~powerup::kSpeedBoost) != 0 || !std::isfinite(worn.paceAdd) ||
        !modifierSlotsFinite(player->actor.save().progress().inventory)) {
        return 0;
    }
    // Preflight only the input-independent slide. Never run PartyMotion's damage,
    // shove, fixture or event callbacks on the live party while making a packet.
    // World bodies may move after this checkpoint; the next host tick corrects it.
    auto actor = player->actor;
    actor.setPaceBonus(worn.paceAdd);
    auto knockback = player->knockback;
    const auto& collision = m_world->collision();
    const auto supported = [&] {
        const auto floor = collision.floorAt(actor.position(), 0.25f, 0.25f);
        return floor && !collision.moving(floor->object) &&
               !collision.floorExitBlocked(floor->object) &&
               std::abs(floor->y - actor.position().y) <= 0.0001f;
    };
    if (!supported()) {
        return 0;
    }
    const auto boxes = m_fixtures.obstacles();
    const auto index = static_cast<usize>(player - m_players.begin());
    const f32 seconds = 1.0f / static_cast<f32>(simulationTickRate());
    u32 ticks = 0;
    for (; ticks < PlayerCombatState::kKnockbackTicks; ++ticks) {
        const Vec3 before = actor.position();
        actor.clearWallContacts();
        actor.slide(knockback.step(seconds, actor.speed()), &collision);
        if (const auto heading = knockback.kick(actor.yaw(), false)) {
            actor.turnTo(*heading);
        }
        actor.update({}, 0, seconds, &collision);
        const auto clearBodies = [&] {
            Vec3 position = actor.position();
            return glm::distance(m_opponents.resolveMovement(actor, before, position), position) <=
                       0.0001f &&
                   !PartyCollision::resolve(m_players, index, before, position);
        };
        if (!clearBodies()) {
            break;
        }
        const Vec3 limited = limitStep(index, before, actor.position());
        if (glm::distance(limited, actor.position()) > 0.00001f) {
            actor.place(before);
            actor.slide(limited - before, &collision);
            if (!clearBodies()) {
                break;
            }
        }
        actor.fall(seconds, collision);
        if (!supported() || std::abs(actor.position().y - before.y) > 0.0001f ||
            std::ranges::any_of(
                actor.wallContacts(),
                [&](const auto& contact) { return collision.moving(contact.object); }) ||
            std::ranges::any_of(boxes, [&](const auto& box) {
                return glm::distance(box.pushOut(actor.position(), actor.radius()),
                                     actor.position()) > 0.0001f;
            })) {
            break;
        }
        knockback.endFrame();
    }
    return ticks;
}

namespace {

constexpr s32 kTurboCrowd = 15;         ///< the swarm in view for the turbo lesson
constexpr f32 kStrongThrowScale = 2.0f; ///< a strong throw's weapon: twice the size and the harm
constexpr std::string_view kTaggedSound = "S_TAGGED";
constexpr f32 kTaggedVolume = 180.0f / 255.0f; ///< fn_8009DCB4

} // namespace

/** What the party's movement asks of the scene as it steps. */
PartyMotion::Events PlayScene::motionEvents() {
    return {
        .perform = [this](usize i, PartyMotion::Action action) { perform(i, action); },
        .select =
            [this](usize i, const SelectorInput& input, s32 elapsed) {
                m_hud.stepSelector(m_players[i].actor, input, elapsed, m_audio,
                                   m_world->ref().isSecret());
            },
        .advanceTurbo =
            [this](usize i, s32 elapsed, f32 duration) {
                m_attacks.updateTurbo(i, elapsed, duration, m_players, [this](s32 id, usize index) {
                    // The meter full, the lesson waits for fifteen in
                    // view (pmotion.c 1756); the combo's for a party
                    // of more than one besides (pmotion.c 1760).
                    const bool crowd = m_opponents.enemies().inView() >= kTurboCrowd;
                    if (id == HelpMessages::kUseCombo) {
                        if (crowd && standingCount() > 1) {
                            postHelp(id, index);
                        }
                    } else if (id != HelpMessages::kUseTurbo || crowd) {
                        postHelp(id, index);
                    }
                });
            },
        .thrownImpact = [this](usize i, f32 damage) { hurt(i, damage, HurtKind::Blow, true); },
        .aim =
            [this](usize i) {
                const PlayerActor& actor = m_players[i].actor;
                return m_attacks.aim(actor, actor.facing(), attackTargets());
            },
        .limitMovement = [this](usize index, const Vec3& before,
                                const Vec3& after) { return limitStep(index, before, after); },
        .attackDeed =
            [this](usize i, bool strong, bool moved, const Vec3& facing) {
                const s32 chain = m_players[i].figure != nullptr
                                      ? m_players[i].figure->animator().meleeChain()
                                      : 0;
                return m_attacks.attackDeed(m_players[i].actor, strong, attackTargets(), moved,
                                            chain, facing);
            },
        .automaticMeleeDeed =
            [this](usize i, const Vec3& facing) {
                if (m_context.config == nullptr || !m_context.config->combat.autoMelee) {
                    return PlayerDeed::None;
                }
                return m_attacks.automaticMeleeDeed(m_players[i].actor, attackTargets(), facing);
            },
        .meleeSense =
            [this](usize i, bool held, const Vec3& facing) {
                return m_attacks.meleeSense(m_players[i].actor, held, attackTargets(), facing);
            },
        .deathContact =
            [this](usize i) { return m_attacks.deathContact(i, m_players, attackTargets()); },
        .grabDeath =
            [this](usize i, s32 ticks, bool allowed) {
                return m_attacks.grabDeath(i, ticks, allowed, m_players, attackTargets());
            },
        .resolveMovement =
            [this](usize i, const Vec3& from, const Vec3& to) {
                return m_opponents.resolveMovement(m_players[i].actor, from, to);
            },
        .startPoint = [this]() -> std::optional<Vec3> {
            if (const WorldLocator* start = m_world->startPoint(0)) {
                return start->position;
            }
            return std::nullopt;
        },
        .comboImpact =
            [this](usize flier, usize thrower, f32 blow) {
                return m_attacks.comboImpact(flier, thrower, blow, m_players, attackTargets());
            }};
}

/** How many of the party stand in the level. */
usize PlayScene::standingCount() const {
    return static_cast<usize>(std::ranges::count_if(m_players, [](const PlayerRuntime& player) {
        return player.life == PlayerLife::Standing;
    }));
}

/** Carries out what a member's movement set off: a throw, a swing, a potion, a footstep. */
void PlayScene::perform(usize i, PartyMotion::Action action) {
    switch (action) {
    case PartyMotion::Action::NoPotion: postHelp(HelpMessages::kNoPotion, i); break;
    case PartyMotion::Action::Ram: m_attacks.ramBarrels(i, m_players, attackTargets()); break;
    case PartyMotion::Action::ThrowWeapon: throwWeapon(m_players[i].actor); break;
    case PartyMotion::Action::FamiliarShot:
        m_arsenal.launchFamiliar(
            m_players[i].actor, m_players[i].figure.get(),
            m_players[i].cursorAiming
                ? std::nullopt
                : m_attacks.aim(m_players[i].actor, m_players[i].actor.facing(), attackTargets()));
        break;
    case PartyMotion::Action::StrongThrow:
        launchWeapon(i, m_players[i].actor.facing(), kStrongThrowScale, true);
        break;
    case PartyMotion::Action::SuperShot:
        m_arsenal.launchSuperShot(
            m_players[i].actor, m_players[i].figure.get(),
            m_players[i].cursorAiming
                ? std::nullopt
                : m_attacks.aim(m_players[i].actor, m_players[i].actor.facing(), attackTargets()));
        break;
    case PartyMotion::Action::ShieldPotion: m_attacks.shieldPotion(i, m_players); break;
    case PartyMotion::Action::ItemAttack: m_attacks.useItemAttack(i, m_players); break;
    case PartyMotion::Action::UsePotion: m_attacks.usePotion(i, m_players); break;
    case PartyMotion::Action::ThrowPotion:
        m_arsenal.throwPotion(
            m_players[i].actor,
            m_players[i].figure != nullptr ? m_players[i].figure->animator().potionThrowTicks() : 0,
            m_players[i].figure != nullptr ? m_players[i].figure->animator().player().generation()
                                           : 0);
        break;
    case PartyMotion::Action::FirstFoot:
    case PartyMotion::Action::SecondFoot: {
        const auto& actor = m_players[i].actor;
        const auto water = m_world->collision().liquidAt(actor.position(), actor.height(), 0);
        m_audio.playFootstep(
            action == PartyMotion::Action::SecondFoot,
            LevelSoundscape::footingOf(m_players[i].floor.flags,
                                       PowerupEffects::of(actor.save().progress().inventory).armor,
                                       water && water->y > actor.position().y));
        break;
    }
    case PartyMotion::Action::Melee: m_attacks.melee(i, m_players, attackTargets()); break;
    case PartyMotion::Action::Tagged:
        postHelp(HelpMessages::kNowIt, i);
        m_audio.playNamed(kTaggedSound, kTaggedVolume);
        break;
    case PartyMotion::Action::ComboStart: m_attacks.comboStart(i, m_players); break;
    case PartyMotion::Action::Fallen:
        m_players[i].mikey.clear();
        m_players[i].mikeyFigure.reset();
        if (m_device != nullptr) {
            PartyPickups::dropKeys(*m_device, *m_world, m_players[i]);
        }
        // Out of the tower the fallen may wait there or leave (inactivate_player).
        m_players[i].towerPrompt = !m_world->isTower();
        break;
    }
}

/** Whether a step keeps the party within the shared view. */
Vec3 PlayScene::limitStep(usize index, const Vec3& before, const Vec3& after) const {
    // A lone player's follow camera can travel with them. The shared
    // view must constrain separation; fixed boss views also need bounds.
    if (!bossCameraOn() && std::ranges::count_if(m_players, [](const PlayerRuntime& player) {
                               return player.life == PlayerLife::Standing;
                           }) <= 1) {
        return after;
    }
    // Use the unshaken gameplay camera, never the promotion/victory cut.
    const auto& camera = bossCameraOn() ? m_bossCamera.camera() : m_camera.camera();
    const auto& attention = bossCameraOn() ? m_bossCamera.attention() : m_camera.attention();
    const auto& actor = m_players[index].actor;
    return CameraMovementLimit::constrain(before, after, attention, camera, cameraView(),
                                          actor.followPoint() - actor.position());
}

} // namespace gdl::game
