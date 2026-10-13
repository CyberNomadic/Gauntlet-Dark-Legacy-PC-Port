#include "game/netplay/CombatSnapshot.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <set>

#include "engine/io/ByteReader.h"

#include "game/combat/DamageTypes.h"
#include "game/netplay/OpponentCheckpoint.h"
#include "game/players/PowerupEffects.h"
#include "game/world/PowerupCompanion.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDLB");
constexpr u32 kVersion = 42;
constexpr u32 kOpponentReplay = 1U << InputCommand::kSeats;
constexpr u32 kCombatReplay = kOpponentReplay << 1U;
void word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
void wide(std::vector<u8>& bytes, u64 value) {
    word(bytes, static_cast<u32>(value));
    word(bytes, static_cast<u32>(value >> 32U));
}
u64 wide(ByteReader& reader) {
    const u64 low = reader.readU32();
    return low | (static_cast<u64>(reader.readU32()) << 32U);
}
void real(std::vector<u8>& bytes, f32 value) {
    word(bytes, std::bit_cast<u32>(value));
}
f32 real(ByteReader& reader) {
    return std::bit_cast<f32>(reader.readU32());
}
bool bounded(f32 value, f32 low, f32 high) {
    return std::isfinite(value) && value >= low && value <= high;
}
bool placementValid(const Mat4& placement) {
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            const f32 limit = column == 3 ? 1'000'000.0f : 1024.0f;
            if (!bounded(placement[column][row], -limit, limit)) {
                return false;
            }
        }
    }
    return placement[0].w == 0 && placement[1].w == 0 && placement[2].w == 0 && placement[3].w == 1;
}
void animation(std::vector<u8>& bytes, const CombatAnimation& value) {
    word(bytes, value.action);
    word(bytes, value.sequence);
    wide(bytes, value.generation);
    real(bytes, value.frame);
    real(bytes, value.transition);
}
CombatAnimation animation(ByteReader& reader) {
    CombatAnimation value;
    value.action = reader.readU32();
    value.sequence = reader.readU32();
    value.generation = wide(reader);
    value.frame = real(reader);
    value.transition = real(reader);
    return value;
}
bool frameValid(const OrdinaryCombatTick::Frame& frame) {
    const auto position = [](const Vec3& value) {
        return bounded(value.x, -1'000'000, 1'000'000) && bounded(value.y, -1'000'000, 1'000'000) &&
               bounded(value.z, -1'000'000, 1'000'000);
    };
    return frame.valid() && position(frame.camera.position) && position(frame.attention) &&
           bounded(frame.camera.pitch, -1'000'000, 1'000'000) &&
           bounded(frame.camera.yaw, -1'000'000, 1'000'000) &&
           bounded(frame.camera.roll, -1'000'000, 1'000'000) && frame.damage.realm >= -1 &&
           frame.damage.realm <= LevelRecord::kMostRealm;
}
void combatFrame(std::vector<u8>& bytes, const OrdinaryCombatTick::Frame& frame) {
    word(bytes, static_cast<u32>(frame.ticks));
    real(bytes, frame.seconds);
    for (const f32 value :
         {frame.camera.position.x, frame.camera.position.y, frame.camera.position.z,
          frame.camera.pitch, frame.camera.yaw, frame.camera.roll, frame.projection.horizontalFov,
          frame.projection.aspect, frame.attention.x, frame.attention.y, frame.attention.z}) {
        real(bytes, value);
    }
    const auto& damage = frame.damage;
    word(bytes, (damage.tower ? 1U : 0U) | (damage.bossEncounter ? 2U : 0U) |
                    (damage.cutscene ? 4U : 0U) | (damage.lastRealmLevel ? 8U : 0U));
    real(bytes, damage.damageScale);
    word(bytes, static_cast<u32>(damage.realm));
    real(bytes, damage.playSeconds);
}
std::optional<OrdinaryCombatTick::Frame> combatFrame(ByteReader& reader) {
    OrdinaryCombatTick::Frame frame;
    frame.ticks = static_cast<s32>(reader.readU32());
    frame.seconds = real(reader);
    frame.camera.position = {real(reader), real(reader), real(reader)};
    frame.camera.pitch = real(reader);
    frame.camera.yaw = real(reader);
    frame.camera.roll = real(reader);
    frame.projection.horizontalFov = real(reader);
    frame.projection.aspect = real(reader);
    frame.attention = {real(reader), real(reader), real(reader)};
    const u32 flags = reader.readU32();
    frame.damage.tower = (flags & 1U) != 0;
    frame.damage.bossEncounter = (flags & 2U) != 0;
    frame.damage.cutscene = (flags & 4U) != 0;
    frame.damage.lastRealmLevel = (flags & 8U) != 0;
    frame.damage.damageScale = real(reader);
    frame.damage.realm = static_cast<s32>(reader.readU32());
    frame.damage.playSeconds = real(reader);
    return (flags & ~15U) == 0 && frameValid(frame) ? std::optional{frame} : std::nullopt;
}
} // namespace

bool CombatAnimation::valid() const {
    return action <= 255 && sequence <= 65535 && bounded(frame, 0, 1'000'000) &&
           bounded(transition, 0, 1) &&
           (generation != 0 || (action == 0 && sequence == 0 && frame == 0 && transition == 1));
}

bool ProjectileState::valid() const {
    if (source > ProjectileSource::PlayerRider || instance == 0 || continuity == 0 ||
        resource == 0 || resource > 65535 || (flags & ~31U) != 0 || !animation.valid() ||
        !bounded(age, 0, 1'000'000) || !bounded(radius, 0, 1024) ||
        !bounded(forward, -1024, 1024) || !bounded(textureFrame, 0, 1'000'000'000.0f) ||
        !bounded(alpha, 0, 1) ||
        (potion &&
         (!potion->valid() || source != ProjectileSource::PlayerEffect || rollback ||
          effectOrigin ||
          (particles && (particles->potion != potion->kind || particles->held != potion->held ||
                         particles->flight.retiring || !particles->potionOrigin ||
                         *particles->potionOrigin != potion->origin)))) ||
        (effectOrigin && (!effectOrigin->valid() || source != ProjectileSource::WorldEffect ||
                          rollback || particles)) ||
        (rollback &&
         (!rollback->valid() ||
          (source != ProjectileSource::Player && source != ProjectileSource::PlayerEffect &&
           source != ProjectileSource::PlayerRider && source != ProjectileSource::Streak))) ||
        (particles &&
         (!particles->valid() ||
          (source != ProjectileSource::PlayerEffect && source != ProjectileSource::PlayerRider) ||
          (particles->potion != 0 && source != ProjectileSource::PlayerEffect) ||
          (particles->flight.retiring && rollback) ||
          (rollback && (particles->potion != 0 || particles->character != rollback->character ||
                        particles->held != rollback->held ||
                        (particles->origin && *particles->origin != rollback->origin)))))) {
        return false;
    }
    return placementValid(placement) && bounded(direction.x, -10000, 10000) &&
           bounded(direction.y, -10000, 10000) && bounded(direction.z, -10000, 10000);
}

bool PlayerWeaponState::valid() const {
    return character <= 16 && resource > 0 && resource <= 65535 && streak <= 65535 &&
           rider <= 65535 && (shots == 1 || shots == 3 || shots == 5) &&
           bounded(hand.x, -100, 100) && bounded(hand.y, -100, 100) && bounded(hand.z, -100, 100) &&
           bounded(speed, 20, 60) && bounded(forward, -1024, 1024) &&
           (!textureFrame || bounded(*textureFrame, 0, 1'000'000'000.0f)) &&
           (!riderTextureFrame ||
            (rider != 0 && bounded(*riderTextureFrame, 0, 1'000'000'000.0f))) &&
           (!target || (bounded(target->x, -1'000'000, 1'000'000) &&
                        bounded(target->y, -1'000'000, 1'000'000) &&
                        bounded(target->z, -1'000'000, 1'000'000)));
}

bool PlayerWeaponModifiers::valid() const {
    const bool fire = (shields & powerup::kFireShield) != 0;
    return (flags != 0 || speed || shields != 0) && PlayerMissiles::replayModifiers(flags) &&
           (shields & ~powerup::kShields) == 0 && ticks <= kMaxTicks &&
           std::isfinite(fireSeconds) && bounded(inventoryStep, 0, kMaxInventoryStep) &&
           (fire ? (fireSeconds == -1 || fireSeconds > 0)
                 : (fireSeconds == 0 && inventoryStep == 0));
}

bool ProjectileState::Rollback::valid() const {
    return origin.valid() && character <= 16 && flight.continuity != 0 &&
           bounded(flight.position.x, -1'000'000, 1'000'000) &&
           bounded(flight.position.y, -1'000'000, 1'000'000) &&
           bounded(flight.position.z, -1'000'000, 1'000'000) &&
           bounded(flight.velocity.x, -10000, 10000) && bounded(flight.velocity.y, -10000, 10000) &&
           bounded(flight.velocity.z, -10000, 10000) && bounded(flight.tumble, -10000, 10000) &&
           bounded(flight.age, 0, PlayerMissiles::kLifeSeconds) &&
           flight.age < PlayerMissiles::kLifeSeconds && bounded(flight.scale, 0.01f, 16);
}

bool ProjectileState::Particles::valid() const {
    if (!flight.valid() || character > 16 || (origin && !origin->valid())) {
        return false;
    }
    if (potion == 0) {
        return !potionOrigin;
    }
    return potion <= 4 && character == 0 && !origin &&
           (!potionOrigin ||
            (potionOrigin->valid() && potionOrigin->kind == PotionIdentity::Kind::Throw));
}

bool ProjectileState::PotionRollback::valid() const {
    return origin.valid() && origin.kind == PotionIdentity::Kind::Throw && kind >= 1 && kind <= 4 &&
           flight.valid();
}

bool PickupState::valid() const {
    return instance != 0 && resource != 0 && resource <= 65535 && continuity != 0 &&
           placementValid(placement) && animation.valid() && animation.action == 0 &&
           animation.transition == 1 && bounded(textureFrame, 0, 1'000'000'000.0f) &&
           bounded(alpha, 0, 1);
}

bool FixtureState::valid() const {
    return source <= FixtureSource::SecretPortal && instance != 0 && resource != 0 &&
           resource <= 65535 && continuity != 0 && placementValid(placement) && pose.valid() &&
           pose.action == 0 && pose.transition == 1 && meshSequence <= 65535 &&
           textureSequence <= 65535 && bounded(meshFrame, 0, 1'000'000) &&
           bounded(textureFrame, 0, 1'000'000) && bounded(textureClock, 0, 1'000'000'000.0f) &&
           bounded(alpha, 0, 1);
}

bool CompanionState::valid(usize slot) const {
    return slot < 2 && form >= 1 && form <= (slot == 0 ? 2U : 7U) && placementValid(placement) &&
           animation.valid() && animation.generation != 0 && animation.action == 0 &&
           animation.transition == 1 && bounded(textureClock, 0, 1'000'000'000.0f) &&
           bounded(alpha, 0, 1) &&
           (!rollback ||
            (slot == 1 && form == static_cast<u32>(PowerupCompanion::Kind::FireShield) &&
             rollback->valid() && rollback->playing && rollback->sequence == 0 &&
             animation.sequence == 0 && rollback->generation == animation.generation));
}

bool PlayerShadowState::valid() const {
    const f32 length = glm::dot(normal, normal);
    return bounded(ground.x, -1'000'000, 1'000'000) && bounded(ground.y, -1'000'000, 1'000'000) &&
           bounded(ground.z, -1'000'000, 1'000'000) && bounded(normal.x, -1, 1) &&
           bounded(normal.y, 0.5f, 1) && bounded(normal.z, -1, 1) &&
           bounded(length, 0.999f, 1.001f) && bounded(alpha, 0, 1);
}

bool PlayerMovementState::valid() const {
    return bounded(speed, 5, 12.5f) && bounded(radius, 0.01f, 50) && bounded(height, 0.01f, 100) &&
           bounded(followHeight, 0, 100) && bounded(cameraYaw, -kTwoPi, kTwoPi) &&
           bounded(attention.x, -1'000'000, 1'000'000) &&
           bounded(attention.y, -1'000'000, 1'000'000) &&
           bounded(attention.z, -1'000'000, 1'000'000) && arrivalMargin >= -120 &&
           arrivalMargin <= 120;
}

bool CombatSnapshot::valid() const {
    if (!motion.valid() || !bounded(worldTextureFrame, 0, 1'000'000'000.0f) || musicArea < -1 ||
        musicArea > 7 || musicLevel > 255 || enemies.size() > kMaxEnemies ||
        projectiles.size() > kMaxProjectiles || pickups.size() > kMaxPickups ||
        fixtures.size() > kMaxFixtures || (geometry && !geometry->valid()) ||
        !FighterPacket::valid(fighters) || (hud && !hud->valid()) ||
        opponents.has_value() != opponentFrame.has_value() ||
        (opponents &&
         (!opponents->valid() || !opponentFrame->valid() ||
          opponentFrame->initialPlayers != static_cast<u32>(opponents->enemies.scales.players) ||
          opponentFrame->initialPlayers != static_cast<u32>(opponents->generators.players)))) {
        return false;
    }
    if (opponentFrame && std::ranges::any_of(opponentFrame->players, [&](const auto& player) {
            return !motion.players[player.seat] || !players[player.seat];
        })) {
        return false;
    }
    if (combatState.has_value() != combatFrame.has_value() ||
        (combatState && (!opponents || !combatState->valid() || !frameValid(*combatFrame) ||
                         !OrdinaryCombatCheckpoint::valid(combatState->combat, *opponents,
                                                          combatState->adapter)))) {
        return false;
    }
    if (combatState) {
        std::array<bool, InputCommand::kSeats> present{};
        for (const auto& native : combatState->combat.players) {
            const auto seat = static_cast<usize>(native.player);
            if (seat >= players.size() || present[seat] || !players[seat] ||
                !motion.players[seat] || native.departed ||
                native.actor.position != motion.players[seat]->position ||
                std::abs(std::remainder(native.actor.yaw - motion.players[seat]->yaw, kTwoPi)) >
                    0.0001f) {
                return false;
            }
            const auto expectedLife =
                native.life == PlayerLife::Standing ? ReplicaPlayerLife::Standing
                : native.life == PlayerLife::Dying  ? ReplicaPlayerLife::Dying
                                                    : ReplicaPlayerLife::InTower;
            const f32 health =
                native.life == PlayerLife::Standing
                    ? std::max(0.0f, static_cast<f32>(native.save.health()) + native.healthFraction)
                    : 0;
            if (players[seat]->life != expectedLife || players[seat]->health != health) {
                return false;
            }
            present[seat] = true;
        }
        for (usize seat = 0; seat < players.size(); ++seat) {
            if (present[seat] != players[seat].has_value()) {
                return false;
            }
        }
        if (combatFrame->ticks != opponentFrame->ticks ||
            combatFrame->seconds != opponentFrame->seconds) {
            return false;
        }
    }
    if (worldMotionTicks > PlatformState::kMaxTicks || (!worldMotion && worldMotionTicks != 0) ||
        (worldMotion && (!worldMotion->valid() || !geometry ||
                         worldMotion->collision.size() != geometry->objectCount))) {
        return false;
    }
    for (usize seat = 0; seat < players.size(); ++seat) {
        const auto& player = players[seat];
        if (hud && hud->players[seat].has_value() != player.has_value()) {
            return false;
        }
        if (player.has_value() != motion.players[seat].has_value() ||
            (player && (!bounded(player->health, 0, 1'000'000) || !player->animation.valid() ||
                        static_cast<u8>(player->life) > 2 || !player->movement.valid() ||
                        ((player->movement.enabled || player->movement.current) &&
                         (player->life != ReplicaPlayerLife::Standing || player->portalPhase))))) {
            return false;
        }
        if (player) {
            if (player->potion &&
                (!player->potion->valid() || !player->rollback ||
                 !PlayerPotionState::supports(*player->rollback) || !player->locomotion ||
                 !player->floor || player->floor->object < 0 || player->floor->placement ||
                 player->movement.platform || !player->movement.current ||
                 !player->movement.enabled || player->reaction != PlayerDeed::None ||
                 player->knockbackTicks != 0 ||
                 (player->modifiers && player->modifiers->shields != 0) ||
                 player->potion->ticks > player->locomotion->ticks ||
                 (player->modifiers && player->potion->ticks > player->modifiers->ticks) ||
                 (player->rollback->animation.transitionTime <
                      player->rollback->animation.transitionLength &&
                  !player->transitionPose))) {
                return false;
            }
            if (player->turboEffects &&
                (!player->turboEffects->valid() || !player->turbo || !player->rollback)) {
                return false;
            }
            if ((player->locomotion &&
                 (!player->locomotion->valid() || !player->movement.enabled || !player->rollback ||
                  player->movement.platform ||
                  player->locomotion->origin != motion.players[seat]->position)) ||
                (player->transitionPose && (!player->transitionPose->valid() || !player->rollback ||
                                            player->rollback->animation.transitionTime >=
                                                player->rollback->animation.transitionLength))) {
                return false;
            }
            if ((player->floor && !player->floor->valid()) ||
                (player->movement.platform &&
                 (!worldMotion || worldMotionTicks == 0 || !player->floor || !player->rollback ||
                  !player->movement.enabled || !player->movement.current ||
                  player->movement.ranged || player->floor->object < 0 ||
                  !player->floor->placement ||
                  static_cast<usize>(player->floor->object) >= worldMotion->collision.size() ||
                  player->melee || player->turbo || player->weapon ||
                  player->knockbackTicks != 0))) {
                return false;
            }
            if ((player->armEquipment & ~(powerup::kShields | powerup::kLeftGauntlet)) != 0) {
                return false;
            }
            if ((player->knockback &&
                 (!Knockback::valid(*player->knockback) || !player->rollback)) ||
                static_cast<u32>(player->reaction) > static_cast<u32>(PlayerDeed::ComboThrown) ||
                player->knockbackTicks > PlayerCombatState::kKnockbackTicks ||
                (!player->knockback &&
                 (player->reaction != PlayerDeed::None || player->knockbackTicks != 0)) ||
                (player->knockbackTicks != 0 &&
                 (!player->movement.enabled || !player->movement.current || player->melee))) {
                return false;
            }
            if (player->melee &&
                (!player->melee->valid() || !player->rollback || !player->movement.enabled ||
                 (player->melee->moving &&
                  (!player->movement.current || player->movement.ranged)))) {
                return false;
            }
            if (player->weapon && !player->weapon->valid()) {
                return false;
            }
            if (player->modifiers && (!player->modifiers->valid() || !player->rollback)) {
                return false;
            }
            PowerupEffects modifiers;
            modifiers.weapon = player->modifiers ? player->modifiers->flags : 0;
            if (player->weapon &&
                (player->weapon->shots != modifiers.shots() ||
                 (player->weapon->rider != 0) != (damage::element(modifiers.weapon) != 0))) {
                return false;
            }
            if ((player->shove && !player->turbo) ||
                player->turbo.has_value() != player->turboGeometry.has_value() ||
                (player->turbo && (!player->turbo->valid() || !player->turboGeometry->valid() ||
                                   !player->turboGeometry->moving || !player->rollback ||
                                   !player->movement.current || !player->movement.enabled ||
                                   player->knockbackTicks != 0))) {
                return false;
            }
            if ((player->rollback && !player->rollback->valid()) ||
                (player->movement.ranged && (!player->movement.enabled || !player->rollback))) {
                return false;
            }
            if (const auto& input = player->rollbackInput;
                input &&
                (!input->valid() || !motion.players[seat] || input->epoch != motion.epoch ||
                 input->seat != seat || input->grant != motion.players[seat]->grant ||
                 input->tick > motion.tick)) {
                return false;
            }
            if (player->shadow &&
                (!player->shadow->valid() || player->life == ReplicaPlayerLife::InTower ||
                 (player->portalPhase && *player->portalPhase >= 1))) {
                return false;
            }
            if (player->portalPhase && (player->life != ReplicaPlayerLife::Standing ||
                                        !bounded(*player->portalPhase, 0, 1))) {
                return false;
            }
            for (usize slot = 0; slot < player->companions.size(); ++slot) {
                const auto& companion = player->companions[slot];
                if (companion &&
                    (!companion->valid(slot) || player->life == ReplicaPlayerLife::InTower)) {
                    return false;
                }
            }
        }
    }
    u64 previous = 0;
    for (const auto& enemy : enemies) {
        if (enemy.instance <= previous || enemy.kind > 65535 || enemy.tier < 1 || enemy.tier > 3 ||
            enemy.variant > 255 || static_cast<u8>(enemy.life) > 2 ||
            !bounded(enemy.health, 0, 1'000'000) || !bounded(enemy.fullHealth, 0, 1'000'000) ||
            !bounded(enemy.position.x, -1'000'000, 1'000'000) ||
            !bounded(enemy.position.y, -1'000'000, 1'000'000) ||
            !bounded(enemy.position.z, -1'000'000, 1'000'000) ||
            !bounded(enemy.yaw, -kTwoPi, kTwoPi) || !enemy.animation.valid() ||
            !bounded(enemy.alpha, 0, 1) || !bounded(enemy.scale, 0, 16) || enemy.deathSkin > 6 ||
            enemy.deathFrame >= 15 || (enemy.deathSkin == 0 && enemy.deathFrame != 0) ||
            (enemy.deathSkin != 0 && enemy.life != ReplicaEnemyLife::Dying)) {
            return false;
        }
        previous = enemy.instance;
    }
    std::set<EffectIdentity> effects;
    std::set<PotionIdentity> potions;
    for (usize i = 0; i < projectiles.size(); ++i) {
        if (!projectiles[i].valid() ||
            (i > 0 && projectiles[i - 1].key() >= projectiles[i].key())) {
            return false;
        }
        if (const auto& rollback = projectiles[i].rollback; rollback) {
            const auto& origin = rollback->origin;
            if (origin.epoch != motion.epoch || !motion.players[origin.seat] ||
                origin.grant != motion.players[origin.seat]->grant) {
                return false;
            }
        }
        if (const auto& particles = projectiles[i].particles; particles && particles->origin) {
            const auto& origin = *particles->origin;
            if (origin.epoch != motion.epoch || !motion.players[origin.seat] ||
                origin.grant != motion.players[origin.seat]->grant) {
                return false;
            }
        }
        if (const auto& origin = projectiles[i].effectOrigin; origin) {
            if (origin->epoch != motion.epoch || !motion.players[origin->seat] ||
                origin->grant != motion.players[origin->seat]->grant ||
                !effects.insert(*origin).second) {
                return false;
            }
        }
        if (const auto* potion = projectiles[i].potionReleaseOrigin(); potion != nullptr) {
            const auto& origin = *potion;
            if (origin.epoch != motion.epoch || !motion.players[origin.seat] ||
                origin.grant != motion.players[origin.seat]->grant ||
                !potions.insert(origin).second) {
                return false;
            }
        }
    }
    previous = 0;
    for (const auto& item : pickups) {
        if (!item.valid() || item.instance <= previous) {
            return false;
        }
        previous = item.instance;
    }
    for (usize i = 0; i < fixtures.size(); ++i) {
        if (!fixtures[i].valid() || (i > 0 && fixtures[i - 1].key() >= fixtures[i].key())) {
            return false;
        }
    }
    return true;
}

std::optional<std::vector<u8>> CombatPacket::encode(const CombatSnapshot& snapshot) {
    if (!snapshot.valid() || std::ranges::any_of(snapshot.projectiles, [&](const auto& shot) {
            return (shot.rollback && shot.rollback->origin.tick > snapshot.motion.tick) ||
                   (shot.particles && shot.particles->origin &&
                    shot.particles->origin->tick > snapshot.motion.tick) ||
                   (shot.effectOrigin && shot.effectOrigin->tick > snapshot.motion.tick) ||
                   (shot.potionReleaseOrigin() &&
                    shot.potionReleaseOrigin()->tick > snapshot.motion.tick);
        })) {
        return std::nullopt;
    }
    const auto motion = MotionPacket::encode(snapshot.motion);
    if (!motion) {
        return std::nullopt;
    }
    const auto fighters = FighterPacket::encode(snapshot.fighters);
    if (!fighters) {
        return std::nullopt;
    }
    const usize fighterBytes = snapshot.fighters.empty() ? 0 : fighters->size();
    const auto hud =
        snapshot.hud ? HudPacket::encode(*snapshot.hud) : std::optional{std::vector<u8>{}};
    if (!hud) {
        return std::nullopt;
    }
    std::vector<u8> melee;
    u32 meleeMask = 0;
    for (usize seat = 0; seat < snapshot.players.size(); ++seat) {
        if (snapshot.players[seat] && snapshot.players[seat]->melee) {
            meleeMask |= 1U << seat;
        }
    }
    if (meleeMask != 0) {
        word(melee, meleeMask);
        for (usize seat = 0; seat < snapshot.players.size(); ++seat) {
            const auto& player = snapshot.players[seat];
            if (player && player->melee) {
                const auto checkpoint = PlayerMeleeCheckpoint::encode(*player->melee);
                if (!checkpoint ||
                    player->melee->selection.feet != snapshot.motion.players[seat]->position) {
                    return std::nullopt;
                }
                word(melee, static_cast<u32>(checkpoint->size()));
                melee.insert(melee.end(), checkpoint->begin(), checkpoint->end());
            }
        }
    }
    std::vector<u8> turbo;
    u32 turboMask = 0;
    for (usize seat = 0; seat < snapshot.players.size(); ++seat) {
        if (snapshot.players[seat] && snapshot.players[seat]->turbo) {
            turboMask |= 1U << seat;
        }
    }
    if (turboMask != 0) {
        word(turbo, turboMask);
        for (usize seat = 0; seat < snapshot.players.size(); ++seat) {
            const auto& player = snapshot.players[seat];
            if (player && player->turbo) {
                const auto timeline = PlayerTurboCheckpoint::encode(*player->turbo);
                const auto region = PlayerMeleeCheckpoint::encode(*player->turboGeometry);
                if (!timeline || !region ||
                    player->turboGeometry->selection.feet !=
                        snapshot.motion.players[seat]->position) {
                    return std::nullopt;
                }
                word(turbo, static_cast<u32>(timeline->size()));
                word(turbo, static_cast<u32>(region->size()));
                turbo.insert(turbo.end(), timeline->begin(), timeline->end());
                turbo.insert(turbo.end(), region->begin(), region->end());
            }
        }
    }
    std::vector<u8> bytes;
    const usize geometryBytes =
        snapshot.geometry
            ? kGeometryHeaderBytes + snapshot.geometry->objects.size() * kGeometryObjectBytes
            : 0;
    PlatformState platform;
    platform.motion = snapshot.worldMotion;
    platform.ticks = snapshot.worldMotionTicks;
    for (usize seat = 0; seat < snapshot.players.size(); ++seat) {
        if (snapshot.players[seat]) {
            platform.floors[seat] = snapshot.players[seat]->floor;
        }
    }
    const bool hasPlatform =
        platform.motion.has_value() ||
        std::ranges::any_of(platform.floors, [](const auto& floor) { return floor.has_value(); });
    const auto platformBytes =
        hasPlatform ? PlatformCheckpoint::encode(platform) : std::optional{std::vector<u8>{}};
    if (!platformBytes) {
        return std::nullopt;
    }
    std::vector<u8> replay;
    u32 replayMask = snapshot.opponents ? kOpponentReplay : 0;
    if (snapshot.combatState) {
        replayMask |= kCombatReplay;
    }
    for (usize seat = 0; seat < snapshot.players.size(); ++seat) {
        const auto& player = snapshot.players[seat];
        if (player && (player->locomotion || player->transitionPose || player->turboEffects ||
                       player->potion)) {
            replayMask |= 1U << seat;
        }
    }
    if (replayMask != 0) {
        word(replay, replayMask);
        for (usize seat = 0; seat < snapshot.players.size(); ++seat) {
            if ((replayMask & (1U << seat)) == 0) {
                continue;
            }
            const auto& player = *snapshot.players[seat];
            const auto contacts = player.locomotion
                                      ? PlayerLocomotionCheckpoint::encode(*player.locomotion)
                                      : std::optional{std::vector<u8>{}};
            const auto pose = player.transitionPose
                                  ? PlayerPoseCheckpoint::encode(*player.transitionPose)
                                  : std::optional{std::vector<u8>{}};
            const auto effects = player.turboEffects
                                     ? TurboEffectCheckpoint::encode(*player.turboEffects)
                                     : std::optional{std::vector<u8>{}};
            const auto potion = player.potion ? PlayerPotionCheckpoint::encode(*player.potion)
                                              : std::optional{std::vector<u8>{}};
            if (!contacts || !pose || !effects || !potion) {
                return std::nullopt;
            }
            word(replay, static_cast<u32>(contacts->size()));
            word(replay, static_cast<u32>(pose->size()));
            word(replay, static_cast<u32>(effects->size()));
            word(replay, static_cast<u32>(potion->size()));
            replay.insert(replay.end(), contacts->begin(), contacts->end());
            replay.insert(replay.end(), pose->begin(), pose->end());
            replay.insert(replay.end(), effects->begin(), effects->end());
            replay.insert(replay.end(), potion->begin(), potion->end());
        }
        if (snapshot.opponents) {
            const auto opponents = OpponentCheckpoint::encode(*snapshot.opponents);
            const auto frame = OpponentFrameCodec::encode(*snapshot.opponentFrame);
            if (!opponents || !frame) {
                return std::nullopt;
            }
            word(replay, static_cast<u32>(opponents->size()));
            word(replay, static_cast<u32>(frame->size()));
            wide(replay, snapshot.motion.epoch);
            wide(replay, snapshot.motion.tick);
            replay.insert(replay.end(), opponents->begin(), opponents->end());
            replay.insert(replay.end(), frame->begin(), frame->end());
        }
        if (snapshot.combatState) {
            const auto combat = CombatStatePacket::encode(*snapshot.combatState);
            if (!combat) {
                return std::nullopt;
            }
            word(replay, static_cast<u32>(combat->size()));
            word(replay, static_cast<u32>(kCombatFrameBytes));
            wide(replay, snapshot.motion.epoch);
            wide(replay, snapshot.motion.tick);
            replay.insert(replay.end(), combat->begin(), combat->end());
            combatFrame(replay, *snapshot.combatFrame);
        }
    }
    const auto playerCount = static_cast<usize>(std::ranges::count_if(
        snapshot.players, [](const auto& player) { return player.has_value(); }));
    const usize requiredBytes =
        kHeaderBytes + motion->size() + playerCount * kPlayerBytes +
        snapshot.enemies.size() * kEnemyBytes + snapshot.projectiles.size() * kProjectileBytes +
        snapshot.pickups.size() * kPickupBytes + snapshot.fixtures.size() * kFixtureBytes +
        geometryBytes + fighterBytes + hud->size() + melee.size() + turbo.size() +
        platformBytes->size() + replay.size();
    if (requiredBytes > kMaxBytes) {
        return std::nullopt;
    }
    bytes.reserve(requiredBytes);
    word(bytes, kMagic);
    word(bytes, kVersion | (static_cast<u32>(snapshot.enemies.size()) << 16U));
    word(bytes, static_cast<u32>(motion->size()));
    word(bytes, static_cast<u32>(snapshot.projectiles.size()));
    word(bytes, static_cast<u32>(geometryBytes));
    word(bytes, static_cast<u32>(snapshot.pickups.size()));
    word(bytes, static_cast<u32>(snapshot.fixtures.size()));
    word(bytes, static_cast<u32>(fighterBytes));
    word(bytes, static_cast<u32>(hud->size()));
    real(bytes, snapshot.worldTextureFrame);
    word(bytes, static_cast<u32>(snapshot.musicArea));
    word(bytes, snapshot.musicLevel);
    word(bytes, static_cast<u32>(melee.size()));
    word(bytes, static_cast<u32>(turbo.size()));
    word(bytes, static_cast<u32>(platformBytes->size()));
    word(bytes, static_cast<u32>(replay.size()));
    bytes.insert(bytes.end(), motion->begin(), motion->end());
    for (const auto& player : snapshot.players) {
        if (player) {
            real(bytes, player->health);
            word(bytes, static_cast<u32>(player->life) | (player->hitFlash ? 4U : 0U) |
                            (player->damageable ? 8U : 0U) | (player->portalPhase ? 16U : 0U) |
                            (player->shadow ? 32U : 0U) | (player->rollback ? 64U : 0U) |
                            (player->rollbackInput ? 128U : 0U) | (player->weapon ? 256U : 0U) |
                            (player->knockback ? 512U : 0U) | (player->weaponHidden ? 1024U : 0U) |
                            (player->shove ? 2048U : 0U) | (player->modifiers ? 4096U : 0U));
            animation(bytes, player->animation);
            real(bytes, player->portalPhase.value_or(0));
            for (const auto& companion : player->companions) {
                if (!companion) {
                    bytes.insert(bytes.end(), kCompanionBytes, 0);
                    continue;
                }
                word(bytes, companion->form);
                for (s32 column = 0; column < 4; ++column) {
                    for (s32 row = 0; row < 3; ++row) {
                        real(bytes, companion->placement[column][row]);
                    }
                }
                animation(bytes, companion->animation);
                real(bytes, companion->textureClock);
                real(bytes, companion->alpha);
                word(bytes, companion->rollback ? 1U : 0U);
                if (companion->rollback) {
                    const auto seed = AnimationCheckpoint::encode(*companion->rollback);
                    if (!seed) {
                        return std::nullopt;
                    }
                    bytes.insert(bytes.end(), seed->begin(), seed->end());
                } else {
                    bytes.insert(bytes.end(), AnimationCheckpoint::kBytes, 0);
                }
            }
            if (player->shadow) {
                const auto& shadow = *player->shadow;
                for (const f32 value :
                     {shadow.ground.x, shadow.ground.y, shadow.ground.z, shadow.normal.x,
                      shadow.normal.y, shadow.normal.z, shadow.alpha}) {
                    real(bytes, value);
                }
            } else {
                bytes.insert(bytes.end(), kShadowBytes, 0);
            }
            const auto& movement = player->movement;
            word(bytes, (movement.enabled ? 1U : 0U) | (movement.cameraLimited ? 2U : 0U) |
                            (movement.ranged ? 4U : 0U) | (movement.current ? 8U : 0U) |
                            (movement.platform ? 16U : 0U));
            for (const f32 value : {movement.speed, movement.radius, movement.height,
                                    movement.followHeight, movement.cameraYaw, movement.attention.x,
                                    movement.attention.y, movement.attention.z}) {
                real(bytes, value);
            }
            wide(bytes, movement.receivedInput);
            word(bytes, static_cast<u32>(movement.arrivalMargin));
            word(bytes, movement.lateInputs);
            if (player->rollback) {
                const auto checkpoint = PlayerActionCheckpoint::encode(*player->rollback);
                if (!checkpoint) {
                    return std::nullopt;
                }
                bytes.insert(bytes.end(), checkpoint->begin(), checkpoint->end());
            } else {
                bytes.insert(bytes.end(), kRollbackBytes, 0);
            }
            if (player->rollbackInput) {
                const auto input = InputPacket::encode(std::span{&*player->rollbackInput, 1});
                if (!input) {
                    return std::nullopt;
                }
                bytes.insert(bytes.end(), input->begin(), input->end());
            } else {
                bytes.insert(bytes.end(), kRollbackInputBytes, 0);
            }
            if (player->weapon) {
                const auto& value = *player->weapon;
                word(bytes, value.character);
                for (const f32 part : {value.hand.x, value.hand.y, value.hand.z, value.speed}) {
                    real(bytes, part);
                }
                word(bytes, value.resource);
                word(bytes, value.streak);
                word(bytes, u32{value.tint.r} | (u32{value.tint.g} << 8U) |
                                (u32{value.tint.b} << 16U) | (u32{value.tint.a} << 24U));
                real(bytes, value.forward);
                word(bytes, (value.target ? 1U : 0U) | (value.textureFrame ? 2U : 0U) |
                                (value.riderTextureFrame ? 4U : 0U));
                const Vec3 target = value.target.value_or(Vec3{0});
                for (const f32 part : {target.x, target.y, target.z}) {
                    real(bytes, part);
                }
                real(bytes, value.textureFrame.value_or(0));
                word(bytes, value.shots);
                word(bytes, value.rider);
                real(bytes, value.riderTextureFrame.value_or(0));
            } else {
                bytes.insert(bytes.end(), kWeaponBytes, 0);
            }
            if (player->knockback) {
                const auto impulse = PlayerKnockbackCheckpoint::encode(*player->knockback);
                if (!impulse) {
                    return std::nullopt;
                }
                bytes.insert(bytes.end(), impulse->begin(), impulse->end());
                word(bytes, static_cast<u32>(player->reaction));
                word(bytes, player->knockbackTicks);
            } else {
                bytes.insert(bytes.end(), kKnockbackBytes, 0);
            }
            word(bytes, player->modifiers ? player->modifiers->flags : 0);
            word(bytes, player->modifiers ? player->modifiers->ticks : 0);
            word(bytes, player->modifiers
                            ? (player->modifiers->speed ? 1U : 0U) | player->modifiers->shields
                            : 0U);
            real(bytes, player->modifiers ? player->modifiers->fireSeconds : 0);
            real(bytes, player->modifiers ? player->modifiers->inventoryStep : 0);
            word(bytes, player->armEquipment);
        }
    }
    for (const auto& enemy : snapshot.enemies) {
        wide(bytes, enemy.instance);
        word(bytes, enemy.kind);
        word(bytes, enemy.tier);
        word(bytes, enemy.variant);
        word(bytes, static_cast<u32>(enemy.life) | (enemy.hitFlash ? 4U : 0U));
        for (const f32 value : {enemy.health, enemy.fullHealth, enemy.position.x, enemy.position.y,
                                enemy.position.z, enemy.yaw}) {
            real(bytes, value);
        }
        animation(bytes, enemy.animation);
        real(bytes, enemy.alpha);
        real(bytes, enemy.scale);
        word(bytes, enemy.deathSkin);
        word(bytes, enemy.deathFrame);
    }
    for (const auto& shot : snapshot.projectiles) {
        word(bytes, static_cast<u32>(shot.source));
        wide(bytes, shot.instance);
        word(bytes, shot.continuity);
        word(bytes, shot.resource);
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 4; ++row) {
                real(bytes, shot.placement[column][row]);
            }
        }
        for (const f32 value : {shot.direction.x, shot.direction.y, shot.direction.z, shot.age,
                                shot.radius, shot.forward}) {
            real(bytes, value);
        }
        word(bytes, u32{shot.tint.r} | (u32{shot.tint.g} << 8U) | (u32{shot.tint.b} << 16U) |
                        (u32{shot.tint.a} << 24U));
        animation(bytes, shot.animation);
        real(bytes, shot.textureFrame);
        real(bytes, shot.alpha);
        word(bytes, shot.flags);
        if (shot.rollback) {
            const auto& value = *shot.rollback;
            word(bytes, 1);
            wide(bytes, value.origin.epoch);
            wide(bytes, value.origin.tick);
            wide(bytes, value.origin.generation);
            word(bytes, value.origin.grant);
            word(bytes, u32{value.origin.seat} | (value.origin.strong ? 256U : 0U) |
                            (value.held ? 512U : 0U) | (u32{value.origin.ordinal} << 16U));
            word(bytes, value.character);
            const auto& flight = value.flight;
            for (const f32 part :
                 {flight.position.x, flight.position.y, flight.position.z, flight.velocity.x,
                  flight.velocity.y, flight.velocity.z, flight.tumble, flight.age, flight.scale}) {
                real(bytes, part);
            }
            word(bytes, flight.continuity);
            word(bytes, value.origin.emission);
        } else {
            bytes.insert(bytes.end(), kShotRollbackBytes, 0);
        }
        if (shot.particles) {
            const auto& value = *shot.particles;
            word(bytes, 1U | (value.origin ? 2U : 0U) | (value.flight.retiring ? 4U : 0U) |
                            (value.held ? 8U : 0U) | (value.potionOrigin ? 16U : 0U));
            word(bytes, value.character | (u32{value.potion} << 8U));
            const auto& birth = value.flight.birth;
            for (const f32 part :
                 {birth.position.x, birth.position.y, birth.position.z, birth.velocity.x,
                  birth.velocity.y, birth.velocity.z, birth.tumble, birth.age, birth.scale}) {
                real(bytes, part);
            }
            word(bytes, birth.continuity);
            real(bytes, value.flight.stepSeconds);
            word(bytes, value.flight.activeSteps);
            word(bytes, value.flight.totalSteps);
            if (value.origin) {
                const auto& origin = *value.origin;
                wide(bytes, origin.epoch);
                wide(bytes, origin.tick);
                wide(bytes, origin.generation);
                word(bytes, origin.grant);
                word(bytes,
                     u32{origin.seat} | (origin.strong ? 256U : 0U) | (u32{origin.ordinal} << 16U));
                word(bytes, origin.emission);
            } else if (value.potionOrigin) {
                const auto& origin = *value.potionOrigin;
                wide(bytes, origin.epoch);
                wide(bytes, origin.tick);
                wide(bytes, origin.generation);
                word(bytes, origin.grant);
                word(bytes, u32{origin.seat} | (static_cast<u32>(origin.kind) << 8U));
                word(bytes, 0);
            } else {
                bytes.insert(bytes.end(), 36, 0);
            }
        } else {
            bytes.insert(bytes.end(), kParticleBytes, 0);
        }
        if (shot.effectOrigin) {
            const auto& origin = *shot.effectOrigin;
            word(bytes, 1);
            wide(bytes, origin.epoch);
            wide(bytes, origin.tick);
            wide(bytes, origin.generation);
            word(bytes, origin.grant);
            word(bytes, u32{origin.seat} | (u32{origin.occurrence} << 8U) |
                            (u32{origin.effectOrdinal} << 16U));
        } else {
            bytes.insert(bytes.end(), kEffectOriginBytes, 0);
        }
        if (shot.potion) {
            const auto& value = *shot.potion;
            word(bytes, 1U | (value.held ? 2U : 0U));
            wide(bytes, value.origin.epoch);
            wide(bytes, value.origin.tick);
            wide(bytes, value.origin.generation);
            word(bytes, value.origin.grant);
            word(bytes, u32{value.origin.seat} | (static_cast<u32>(value.origin.kind) << 8U));
            word(bytes, value.kind);
            const auto& flight = value.flight;
            for (const f32 part :
                 {flight.position.x, flight.position.y, flight.position.z, flight.velocity.x,
                  flight.velocity.y, flight.velocity.z, flight.tumble, flight.age, flight.scale}) {
                real(bytes, part);
            }
            word(bytes, flight.continuity);
        } else {
            bytes.insert(bytes.end(), kPotionRollbackBytes, 0);
        }
    }
    for (const auto& item : snapshot.pickups) {
        wide(bytes, item.instance);
        word(bytes, item.resource);
        word(bytes, item.continuity);
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 3; ++row) {
                real(bytes, item.placement[column][row]);
            }
        }
        animation(bytes, item.animation);
        real(bytes, item.textureFrame);
        real(bytes, item.alpha);
    }
    for (const auto& fixture : snapshot.fixtures) {
        word(bytes, static_cast<u32>(fixture.source));
        wide(bytes, fixture.instance);
        word(bytes, fixture.resource);
        word(bytes, fixture.continuity);
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 3; ++row) {
                real(bytes, fixture.placement[column][row]);
            }
        }
        animation(bytes, fixture.pose);
        word(bytes, fixture.meshSequence);
        real(bytes, fixture.meshFrame);
        word(bytes, fixture.textureSequence);
        real(bytes, fixture.textureFrame);
        real(bytes, fixture.textureClock);
        real(bytes, fixture.alpha);
        word(bytes, fixture.cameraFacing ? 1 : 0);
    }
    if (snapshot.geometry) {
        const auto& geometry = *snapshot.geometry;
        wide(bytes, geometry.layout);
        word(bytes, geometry.objectCount);
        word(bytes, static_cast<u32>(geometry.objects.size()));
        real(bytes, geometry.darken);
        for (const auto& object : geometry.objects) {
            word(bytes, object.index);
            word(bytes, object.continuity);
            // Affine bottom row is implicit: 0, 0, 0, 1. No matrix memory copies.
            for (s32 column = 0; column < 4; ++column) {
                for (s32 row = 0; row < 3; ++row) {
                    real(bytes, object.local[column][row]);
                }
            }
            real(bytes, object.alpha);
            word(bytes, object.visible ? 1 : 0);
        }
    }
    if (fighterBytes != 0) {
        bytes.insert(bytes.end(), fighters->begin(), fighters->end());
    }
    bytes.insert(bytes.end(), hud->begin(), hud->end());
    bytes.insert(bytes.end(), melee.begin(), melee.end());
    bytes.insert(bytes.end(), turbo.begin(), turbo.end());
    bytes.insert(bytes.end(), platformBytes->begin(), platformBytes->end());
    bytes.insert(bytes.end(), replay.begin(), replay.end());
    return bytes;
}

std::optional<CombatSnapshot> CombatPacket::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    if (reader.readU32() != kMagic || reader.readU16() != kVersion) {
        return std::nullopt;
    }
    const usize enemies = reader.readU16();
    const usize motionBytes = reader.readU32();
    const usize projectiles = reader.readU32();
    const usize geometryBytes = reader.readU32();
    const usize pickups = reader.readU32();
    const usize fixtures = reader.readU32();
    const usize fighterBytes = reader.readU32();
    const usize hudBytes = reader.readU32();
    const f32 worldTextureFrame = real(reader);
    const s32 musicArea = static_cast<s32>(reader.readU32());
    const u32 musicLevel = reader.readU32();
    const usize meleeBytes = reader.readU32();
    const usize turboBytes = reader.readU32();
    const usize platformBytes = reader.readU32();
    const usize replayBytes = reader.readU32();
    if (enemies > CombatSnapshot::kMaxEnemies || projectiles > CombatSnapshot::kMaxProjectiles ||
        pickups > CombatSnapshot::kMaxPickups || fixtures > CombatSnapshot::kMaxFixtures ||
        motionBytes > reader.remaining() || fighterBytes > FighterPacket::kMaxBytes ||
        hudBytes > HudPacket::kMaxBytes || meleeBytes > kMeleeBytes ||
        (meleeBytes != 0 && meleeBytes < 4) || turboBytes > kTurboBytes ||
        (turboBytes != 0 && turboBytes < 4) || platformBytes > PlatformCheckpoint::kMaxBytes ||
        (platformBytes != 0 && platformBytes < PlatformCheckpoint::kHeaderBytes) ||
        replayBytes > kReplayBytes + kOpponentHeaderBytes + OpponentCheckpoint::kMaxBytes +
                          OpponentFrameCodec::kMaxBytes + kCombatHeaderBytes +
                          CombatStatePacket::kMaxBytes + kCombatFrameBytes ||
        (replayBytes != 0 && replayBytes < 4) ||
        (geometryBytes != 0 && (geometryBytes < kGeometryHeaderBytes ||
                                geometryBytes > kGeometryHeaderBytes + SceneGeometry::kMaxStates *
                                                                           kGeometryObjectBytes))) {
        return std::nullopt;
    }
    const auto motion = MotionPacket::decode(bytes.subspan(kHeaderBytes, motionBytes));
    if (!motion) {
        return std::nullopt;
    }
    CombatSnapshot result;
    result.motion = *motion;
    result.worldTextureFrame = worldTextureFrame;
    result.musicArea = musicArea;
    result.musicLevel = musicLevel;
    usize players = 0;
    for (const auto& player : motion->players) {
        players += player ? 1 : 0;
    }
    if (bytes.size() != kHeaderBytes + motionBytes + players * kPlayerBytes +
                            enemies * kEnemyBytes + projectiles * kProjectileBytes +
                            pickups * kPickupBytes + fixtures * kFixtureBytes + geometryBytes +
                            fighterBytes + hudBytes + meleeBytes + turboBytes + platformBytes +
                            replayBytes) {
        return std::nullopt;
    }
    ByteReader data(bytes.subspan(kHeaderBytes + motionBytes));
    for (usize seat = 0; seat < result.players.size(); ++seat) {
        if (motion->players[seat]) {
            PlayerCombatState player;
            player.health = real(data);
            const u32 flags = data.readU32();
            if ((flags & ~8191U) != 0) {
                return std::nullopt;
            }
            player.life = static_cast<ReplicaPlayerLife>(flags & 3U);
            player.hitFlash = (flags & 4U) != 0;
            player.damageable = (flags & 8U) != 0;
            player.weaponHidden = (flags & 1024U) != 0;
            player.shove = (flags & 2048U) != 0;
            player.animation = animation(data);
            const f32 portalPhase = real(data);
            if ((flags & 16U) != 0) {
                player.portalPhase = portalPhase;
            } else if (portalPhase != 0) {
                return std::nullopt;
            }
            for (auto& companion : player.companions) {
                CompanionState value;
                value.form = data.readU32();
                if (value.form == 0) {
                    for (usize wordIndex = 1; wordIndex < kCompanionBytes / 4; ++wordIndex) {
                        if (data.readU32() != 0) {
                            return std::nullopt;
                        }
                    }
                    continue;
                }
                for (s32 column = 0; column < 4; ++column) {
                    for (s32 row = 0; row < 3; ++row) {
                        value.placement[column][row] = real(data);
                    }
                }
                value.animation = animation(data);
                value.textureClock = real(data);
                value.alpha = real(data);
                const u32 seeded = data.readU32();
                const auto seed = data.readBytes(AnimationCheckpoint::kBytes);
                if (seeded > 1) {
                    return std::nullopt;
                }
                if (seeded != 0) {
                    value.rollback = AnimationCheckpoint::decode(seed);
                    if (!value.rollback) {
                        return std::nullopt;
                    }
                } else if (std::ranges::any_of(seed, [](u8 byte) { return byte != 0; })) {
                    return std::nullopt;
                }
                companion = value;
            }
            if ((flags & 32U) != 0) {
                PlayerShadowState shadow;
                shadow.ground = {real(data), real(data), real(data)};
                shadow.normal = {real(data), real(data), real(data)};
                shadow.alpha = real(data);
                player.shadow = shadow;
            } else {
                for (usize i = 0; i < kShadowBytes / 4; ++i) {
                    if (data.readU32() != 0) {
                        return std::nullopt;
                    }
                }
            }
            auto& movement = player.movement;
            const u32 movementFlags = data.readU32();
            if ((movementFlags & ~31U) != 0) {
                return std::nullopt;
            }
            movement.enabled = (movementFlags & 1U) != 0;
            movement.cameraLimited = (movementFlags & 2U) != 0;
            movement.ranged = (movementFlags & 4U) != 0;
            movement.current = (movementFlags & 8U) != 0;
            movement.platform = (movementFlags & 16U) != 0;
            movement.speed = real(data);
            movement.radius = real(data);
            movement.height = real(data);
            movement.followHeight = real(data);
            movement.cameraYaw = real(data);
            movement.attention = {real(data), real(data), real(data)};
            movement.receivedInput = wide(data);
            movement.arrivalMargin = std::bit_cast<s32>(data.readU32());
            movement.lateInputs = data.readU32();
            const auto checkpoint = data.readBytes(kRollbackBytes);
            if ((flags & 64U) != 0) {
                player.rollback = PlayerActionCheckpoint::decode(checkpoint);
                if (!player.rollback) {
                    return std::nullopt;
                }
            } else if (std::ranges::any_of(checkpoint, [](u8 value) { return value != 0; })) {
                return std::nullopt;
            }
            const auto inputBytes = data.readBytes(kRollbackInputBytes);
            if ((flags & 128U) != 0) {
                const auto input = InputPacket::decode(inputBytes);
                if (!input || input->size() != 1) {
                    return std::nullopt;
                }
                player.rollbackInput = input->front();
            } else if (std::ranges::any_of(inputBytes, [](u8 value) { return value != 0; })) {
                return std::nullopt;
            }
            if ((flags & 256U) != 0) {
                PlayerWeaponState value;
                value.character = data.readU32();
                value.hand = {real(data), real(data), real(data)};
                value.speed = real(data);
                value.resource = data.readU32();
                value.streak = data.readU32();
                const u32 tint = data.readU32();
                value.tint =
                    Color::rgba(static_cast<u8>(tint), static_cast<u8>(tint >> 8U),
                                static_cast<u8>(tint >> 16U), static_cast<u8>(tint >> 24U));
                value.forward = real(data);
                const u32 targeted = data.readU32();
                const Vec3 target{real(data), real(data), real(data)};
                const f32 textureFrame = real(data);
                const u32 shots = data.readU32();
                value.rider = data.readU32();
                const u32 riderTextureBits = data.readU32();
                const f32 riderTextureFrame = std::bit_cast<f32>(riderTextureBits);
                if (shots != 1 && shots != 3 && shots != 5) {
                    return std::nullopt;
                }
                value.shots = static_cast<u8>(shots);
                if (targeted > 7 || ((targeted & 1U) == 0 && target != Vec3{0}) ||
                    ((targeted & 2U) == 0 && textureFrame != 0) ||
                    ((targeted & 4U) == 0 && riderTextureBits != 0)) {
                    return std::nullopt;
                }
                if ((targeted & 1U) != 0) {
                    value.target = target;
                }
                if ((targeted & 2U) != 0) {
                    value.textureFrame = textureFrame;
                }
                if ((targeted & 4U) != 0) {
                    value.riderTextureFrame = riderTextureFrame;
                }
                player.weapon = value;
            } else {
                for (usize i = 0; i < kWeaponBytes / 4; ++i) {
                    if (data.readU32() != 0) {
                        return std::nullopt;
                    }
                }
            }
            const auto impulse = data.readBytes(kKnockbackBytes);
            if ((flags & 512U) != 0) {
                player.knockback = PlayerKnockbackCheckpoint::decode(
                    impulse.first(PlayerKnockbackCheckpoint::kBytes));
                ByteReader tail(impulse.last(8));
                const u32 reaction = tail.readU32();
                if (!player.knockback || reaction > static_cast<u32>(PlayerDeed::ComboThrown)) {
                    return std::nullopt;
                }
                player.reaction = static_cast<PlayerDeed>(reaction);
                player.knockbackTicks = tail.readU32();
            } else if (std::ranges::any_of(impulse, [](u8 value) { return value != 0; })) {
                return std::nullopt;
            }
            PlayerWeaponModifiers modifiers{data.readU32(), data.readU32()};
            const u32 effects = data.readU32();
            if ((effects & ~(1U | powerup::kShields)) != 0) {
                return std::nullopt;
            }
            modifiers.speed = (effects & 1U) != 0;
            modifiers.shields = effects & powerup::kShields;
            modifiers.fireSeconds = real(data);
            modifiers.inventoryStep = real(data);
            if ((flags & 4096U) != 0) {
                player.modifiers = modifiers;
            } else if (modifiers.flags != 0 || modifiers.ticks != 0 || modifiers.speed ||
                       modifiers.shields != 0 || std::bit_cast<u32>(modifiers.fireSeconds) != 0 ||
                       std::bit_cast<u32>(modifiers.inventoryStep) != 0) {
                return std::nullopt;
            }
            player.armEquipment = data.readU32();
            result.players[seat] = player;
        }
    }
    result.enemies.reserve(enemies);
    for (usize i = 0; i < enemies; ++i) {
        EnemyCombatState enemy;
        enemy.instance = wide(data);
        enemy.kind = data.readU32();
        enemy.tier = data.readU32();
        enemy.variant = data.readU32();
        const u32 flags = data.readU32();
        if ((flags & ~7U) != 0) {
            return std::nullopt;
        }
        enemy.life = static_cast<ReplicaEnemyLife>(flags & 3U);
        enemy.hitFlash = (flags & 4U) != 0;
        enemy.health = real(data);
        enemy.fullHealth = real(data);
        enemy.position.x = real(data);
        enemy.position.y = real(data);
        enemy.position.z = real(data);
        enemy.yaw = real(data);
        enemy.animation = animation(data);
        enemy.alpha = real(data);
        enemy.scale = real(data);
        enemy.deathSkin = data.readU32();
        enemy.deathFrame = data.readU32();
        result.enemies.push_back(enemy);
    }
    result.projectiles.reserve(projectiles);
    for (usize i = 0; i < projectiles; ++i) {
        ProjectileState shot;
        const u32 source = data.readU32();
        if (source > static_cast<u32>(ProjectileSource::PlayerRider)) {
            return std::nullopt;
        }
        shot.source = static_cast<ProjectileSource>(source);
        shot.instance = wide(data);
        shot.continuity = data.readU32();
        shot.resource = data.readU32();
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 4; ++row) {
                shot.placement[column][row] = real(data);
            }
        }
        shot.direction.x = real(data);
        shot.direction.y = real(data);
        shot.direction.z = real(data);
        shot.age = real(data);
        shot.radius = real(data);
        shot.forward = real(data);
        const u32 tint = data.readU32();
        shot.tint = Color::rgba(static_cast<u8>(tint), static_cast<u8>(tint >> 8U),
                                static_cast<u8>(tint >> 16U), static_cast<u8>(tint >> 24U));
        shot.animation = animation(data);
        shot.textureFrame = real(data);
        shot.alpha = real(data);
        shot.flags = data.readU32();
        const u32 rollback = data.readU32();
        if (rollback == 1) {
            ProjectileState::Rollback value;
            value.origin.epoch = wide(data);
            value.origin.tick = wide(data);
            value.origin.generation = wide(data);
            value.origin.grant = data.readU32();
            const u32 identity = data.readU32();
            if ((identity & ~(771U | (7U << 16U))) != 0) {
                return std::nullopt;
            }
            value.origin.seat = static_cast<u8>(identity & 3U);
            value.origin.strong = (identity & 256U) != 0;
            value.origin.ordinal = static_cast<u8>((identity >> 16U) & 7U);
            value.held = (identity & 512U) != 0;
            value.character = data.readU32();
            value.flight.position = {real(data), real(data), real(data)};
            value.flight.velocity = {real(data), real(data), real(data)};
            value.flight.tumble = real(data);
            value.flight.age = real(data);
            value.flight.scale = real(data);
            value.flight.continuity = data.readU32();
            value.origin.emission = data.readU32();
            shot.rollback = value;
        } else if (rollback == 0) {
            for (usize j = 1; j < kShotRollbackBytes / 4; ++j) {
                if (data.readU32() != 0) {
                    return std::nullopt;
                }
            }
        } else {
            return std::nullopt;
        }
        const u32 particles = data.readU32();
        if ((particles & 1U) != 0 && (particles & ~31U) == 0 && (particles & 18U) != 18U) {
            ProjectileState::Particles value;
            const u32 kind = data.readU32();
            if ((kind & ~2047U) != 0) {
                return std::nullopt;
            }
            value.character = kind & 255U;
            value.potion = static_cast<u8>((kind >> 8U) & 7U);
            auto& birth = value.flight.birth;
            birth.position = {real(data), real(data), real(data)};
            birth.velocity = {real(data), real(data), real(data)};
            birth.tumble = real(data);
            birth.age = real(data);
            birth.scale = real(data);
            birth.continuity = data.readU32();
            value.flight.stepSeconds = real(data);
            value.flight.activeSteps = data.readU32();
            value.flight.totalSteps = data.readU32();
            value.flight.retiring = (particles & 4U) != 0;
            value.held = (particles & 8U) != 0;
            if ((particles & 2U) != 0) {
                ShotIdentity origin;
                origin.epoch = wide(data);
                origin.tick = wide(data);
                origin.generation = wide(data);
                origin.grant = data.readU32();
                const u32 identity = data.readU32();
                if ((identity & ~(259U | (7U << 16U))) != 0) {
                    return std::nullopt;
                }
                origin.seat = static_cast<u8>(identity & 3U);
                origin.strong = (identity & 256U) != 0;
                origin.ordinal = static_cast<u8>((identity >> 16U) & 7U);
                origin.emission = data.readU32();
                value.origin = origin;
            } else if ((particles & 16U) != 0) {
                PotionIdentity origin;
                origin.epoch = wide(data);
                origin.tick = wide(data);
                origin.generation = wide(data);
                origin.grant = data.readU32();
                const u32 identity = data.readU32();
                if ((identity & ~259U) != 0 || data.readU32() != 0) {
                    return std::nullopt;
                }
                origin.seat = static_cast<u8>(identity & 3U);
                origin.kind = static_cast<PotionIdentity::Kind>((identity >> 8U) & 1U);
                value.potionOrigin = origin;
            } else {
                for (usize j = 0; j < 9; ++j) {
                    if (data.readU32() != 0) {
                        return std::nullopt;
                    }
                }
            }
            shot.particles = value;
        } else if (particles == 0) {
            for (usize j = 1; j < kParticleBytes / 4; ++j) {
                if (data.readU32() != 0) {
                    return std::nullopt;
                }
            }
        } else {
            return std::nullopt;
        }
        const u32 effect = data.readU32();
        if (effect == 1) {
            EffectIdentity origin;
            origin.epoch = wide(data);
            origin.tick = wide(data);
            origin.generation = wide(data);
            origin.grant = data.readU32();
            const u32 coordinates = data.readU32();
            if ((coordinates & ~(3U | (127U << 8U) | (127U << 16U))) != 0) {
                return std::nullopt;
            }
            origin.seat = static_cast<u8>(coordinates & 3U);
            origin.occurrence = static_cast<u16>((coordinates >> 8U) & 127U);
            origin.effectOrdinal = static_cast<u16>((coordinates >> 16U) & 127U);
            shot.effectOrigin = origin;
        } else if (effect == 0) {
            for (usize j = 1; j < kEffectOriginBytes / 4; ++j) {
                if (data.readU32() != 0) {
                    return std::nullopt;
                }
            }
        } else {
            return std::nullopt;
        }
        const u32 potion = data.readU32();
        if (potion == 1 || potion == 3) {
            ProjectileState::PotionRollback value;
            value.held = (potion & 2U) != 0;
            value.origin.epoch = wide(data);
            value.origin.tick = wide(data);
            value.origin.generation = wide(data);
            value.origin.grant = data.readU32();
            const u32 identity = data.readU32();
            const u32 kind = data.readU32();
            if ((identity & ~(3U | (1U << 8U))) != 0 || kind < 1 || kind > 4) {
                return std::nullopt;
            }
            value.origin.seat = static_cast<u8>(identity & 3U);
            value.origin.kind = static_cast<PotionIdentity::Kind>((identity >> 8U) & 1U);
            value.kind = static_cast<u8>(kind);
            value.flight.position = {real(data), real(data), real(data)};
            value.flight.velocity = {real(data), real(data), real(data)};
            value.flight.tumble = real(data);
            value.flight.age = real(data);
            value.flight.scale = real(data);
            value.flight.continuity = data.readU32();
            shot.potion = value;
        } else if (potion == 0) {
            for (usize j = 1; j < kPotionRollbackBytes / 4; ++j) {
                if (data.readU32() != 0) {
                    return std::nullopt;
                }
            }
        } else {
            return std::nullopt;
        }
        result.projectiles.push_back(shot);
    }
    result.pickups.reserve(pickups);
    for (usize i = 0; i < pickups; ++i) {
        PickupState item;
        item.instance = wide(data);
        item.resource = data.readU32();
        item.continuity = data.readU32();
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 3; ++row) {
                item.placement[column][row] = real(data);
            }
        }
        item.animation = animation(data);
        item.textureFrame = real(data);
        item.alpha = real(data);
        result.pickups.push_back(item);
    }
    result.fixtures.reserve(fixtures);
    for (usize i = 0; i < fixtures; ++i) {
        FixtureState fixture;
        const auto source = data.readU32();
        if (source > static_cast<u32>(FixtureSource::SecretPortal)) {
            return std::nullopt;
        }
        fixture.source = static_cast<FixtureSource>(source);
        fixture.instance = wide(data);
        fixture.resource = data.readU32();
        fixture.continuity = data.readU32();
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 3; ++row) {
                fixture.placement[column][row] = real(data);
            }
        }
        fixture.pose = animation(data);
        fixture.meshSequence = data.readU32();
        fixture.meshFrame = real(data);
        fixture.textureSequence = data.readU32();
        fixture.textureFrame = real(data);
        fixture.textureClock = real(data);
        fixture.alpha = real(data);
        const auto flags = data.readU32();
        if (flags > 1) {
            return std::nullopt;
        }
        fixture.cameraFacing = flags != 0;
        result.fixtures.push_back(fixture);
    }
    if (geometryBytes != 0) {
        SceneGeometry geometry;
        geometry.layout = wide(data);
        geometry.objectCount = data.readU32();
        const usize count = data.readU32();
        geometry.darken = real(data);
        if (count > SceneGeometry::kMaxStates ||
            geometryBytes != kGeometryHeaderBytes + count * kGeometryObjectBytes) {
            return std::nullopt;
        }
        geometry.objects.reserve(count);
        for (usize i = 0; i < count; ++i) {
            SceneGeometry::Object object;
            object.index = data.readU32();
            object.continuity = data.readU32();
            for (s32 column = 0; column < 4; ++column) {
                for (s32 row = 0; row < 3; ++row) {
                    object.local[column][row] = real(data);
                }
            }
            object.alpha = real(data);
            const u32 visible = data.readU32();
            if (visible > 1) {
                return std::nullopt;
            }
            object.visible = visible != 0;
            geometry.objects.push_back(object);
        }
        result.geometry = std::move(geometry);
    }
    if (fighterBytes != 0) {
        auto fighters = FighterPacket::decode(data.readBytes(fighterBytes));
        if (!fighters || fighters->empty()) {
            return std::nullopt;
        }
        result.fighters = std::move(*fighters);
    }
    if (hudBytes != 0) {
        result.hud = HudPacket::decode(data.readBytes(hudBytes));
        if (!result.hud) {
            return std::nullopt;
        }
    }
    if (meleeBytes != 0) {
        ByteReader melee(data.readBytes(meleeBytes));
        const u32 mask = melee.readU32();
        if (mask == 0 || mask >= (1U << InputCommand::kSeats)) {
            return std::nullopt;
        }
        for (usize seat = 0; seat < result.players.size(); ++seat) {
            if ((mask & (1U << seat)) == 0) {
                continue;
            }
            if (!result.players[seat] || melee.remaining() < 4) {
                return std::nullopt;
            }
            const usize size = melee.readU32();
            if (size > melee.remaining()) {
                return std::nullopt;
            }
            auto checkpoint = PlayerMeleeCheckpoint::decode(melee.readBytes(size));
            if (!checkpoint ||
                checkpoint->selection.feet != result.motion.players[seat]->position) {
                return std::nullopt;
            }
            result.players[seat]->melee = std::move(checkpoint);
        }
        if (!melee.atEnd()) {
            return std::nullopt;
        }
    }
    if (turboBytes != 0) {
        ByteReader turbo(data.readBytes(turboBytes));
        const u32 mask = turbo.readU32();
        if (mask == 0 || mask >= (1U << InputCommand::kSeats)) {
            return std::nullopt;
        }
        for (usize seat = 0; seat < result.players.size(); ++seat) {
            if ((mask & (1U << seat)) == 0) {
                continue;
            }
            if (!result.players[seat] || turbo.remaining() < 8) {
                return std::nullopt;
            }
            const usize timelineBytes = turbo.readU32();
            const usize regionBytes = turbo.readU32();
            if (timelineBytes > PlayerTurboCheckpoint::kMaxBytes ||
                regionBytes > PlayerMeleeCheckpoint::kMaxBytes ||
                timelineBytes + regionBytes > turbo.remaining()) {
                return std::nullopt;
            }
            auto timeline = PlayerTurboCheckpoint::decode(turbo.readBytes(timelineBytes));
            auto region = PlayerMeleeCheckpoint::decode(turbo.readBytes(regionBytes));
            if (!timeline || !region ||
                region->selection.feet != result.motion.players[seat]->position) {
                return std::nullopt;
            }
            result.players[seat]->turbo = std::move(timeline);
            result.players[seat]->turboGeometry = std::move(region);
        }
        if (!turbo.atEnd()) {
            return std::nullopt;
        }
    }
    if (platformBytes != 0) {
        auto platform = PlatformCheckpoint::decode(data.readBytes(platformBytes));
        if (!platform ||
            (!platform->motion && std::ranges::none_of(platform->floors, [](const auto& floor) {
                return floor.has_value();
            }))) {
            return std::nullopt;
        }
        for (usize seat = 0; seat < platform->floors.size(); ++seat) {
            if (!result.players[seat] && platform->floors[seat]) {
                return std::nullopt;
            }
            if (result.players[seat]) {
                result.players[seat]->floor = platform->floors[seat];
            }
        }
        result.worldMotion = std::move(platform->motion);
        result.worldMotionTicks = platform->ticks;
    }
    if (replayBytes != 0) {
        ByteReader replay(data.readBytes(replayBytes));
        const u32 mask = replay.readU32();
        if (mask == 0 || (mask & ~(15U | kOpponentReplay | kCombatReplay)) != 0) {
            return std::nullopt;
        }
        for (usize seat = 0; seat < result.players.size(); ++seat) {
            if ((mask & (1U << seat)) == 0) {
                continue;
            }
            if (!result.players[seat] || replay.remaining() < 16) {
                return std::nullopt;
            }
            const usize contacts = replay.readU32();
            const usize pose = replay.readU32();
            const usize effects = replay.readU32();
            const usize potion = replay.readU32();
            if (contacts > PlayerLocomotionCheckpoint::kMaxBytes ||
                pose > PlayerPoseCheckpoint::kMaxBytes ||
                effects > TurboEffectCheckpoint::kMaxBytes ||
                potion > PlayerPotionCheckpoint::kMaxBytes ||
                contacts + pose + effects + potion == 0 ||
                contacts + pose + effects + potion > replay.remaining()) {
                return std::nullopt;
            }
            auto& player = *result.players[seat];
            if (contacts != 0) {
                player.locomotion = PlayerLocomotionCheckpoint::decode(replay.readBytes(contacts));
                if (!player.locomotion) {
                    return std::nullopt;
                }
            }
            if (pose != 0) {
                player.transitionPose = PlayerPoseCheckpoint::decode(replay.readBytes(pose));
                if (!player.transitionPose) {
                    return std::nullopt;
                }
            }
            if (effects != 0) {
                player.turboEffects = TurboEffectCheckpoint::decode(replay.readBytes(effects));
                if (!player.turboEffects) {
                    return std::nullopt;
                }
            }
            if (potion != 0) {
                player.potion = PlayerPotionCheckpoint::decode(replay.readBytes(potion));
                if (!player.potion) {
                    return std::nullopt;
                }
            }
        }
        if ((mask & kOpponentReplay) != 0) {
            if (replay.remaining() < kOpponentHeaderBytes) {
                return std::nullopt;
            }
            const usize size = replay.readU32();
            const usize frameSize = replay.readU32();
            const u64 epoch = wide(replay);
            const u64 tick = wide(replay);
            if (size < OpponentCheckpoint::kHeaderBytes || size > OpponentCheckpoint::kMaxBytes ||
                frameSize < OpponentFrameCodec::kHeaderBytes ||
                frameSize > OpponentFrameCodec::kMaxBytes || size > replay.remaining() ||
                frameSize > replay.remaining() - size || epoch != result.motion.epoch ||
                tick != result.motion.tick) {
                return std::nullopt;
            }
            result.opponents = OpponentCheckpoint::decode(replay.readBytes(size));
            result.opponentFrame = OpponentFrameCodec::decode(replay.readBytes(frameSize));
            if (!result.opponents || !result.opponentFrame) {
                return std::nullopt;
            }
        }
        if ((mask & kCombatReplay) != 0) {
            if ((mask & kOpponentReplay) == 0 || replay.remaining() < kCombatHeaderBytes) {
                return std::nullopt;
            }
            const usize size = replay.readU32();
            const usize frameSize = replay.readU32();
            const u64 epoch = wide(replay);
            const u64 tick = wide(replay);
            if (size < CombatStatePacket::kHeaderBytes || size > CombatStatePacket::kMaxBytes ||
                frameSize != kCombatFrameBytes || size > replay.remaining() ||
                frameSize > replay.remaining() - size || epoch != result.motion.epoch ||
                tick != result.motion.tick) {
                return std::nullopt;
            }
            result.combatState = CombatStatePacket::decode(replay.readBytes(size));
            result.combatFrame = combatFrame(replay);
            if (!result.combatState || !result.combatFrame) {
                return std::nullopt;
            }
        }
        if (!replay.atEnd()) {
            return std::nullopt;
        }
    }
    if (!data.atEnd()) {
        return std::nullopt;
    }
    if (std::ranges::any_of(result.projectiles, [&](const auto& shot) {
            return (shot.rollback && shot.rollback->origin.tick > result.motion.tick) ||
                   (shot.particles && shot.particles->origin &&
                    shot.particles->origin->tick > result.motion.tick) ||
                   (shot.effectOrigin && shot.effectOrigin->tick > result.motion.tick) ||
                   (shot.potionReleaseOrigin() &&
                    shot.potionReleaseOrigin()->tick > result.motion.tick);
        })) {
        return std::nullopt;
    }
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}

} // namespace gdl::game
