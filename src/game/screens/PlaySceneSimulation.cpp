#include <bit>
#include <string_view>

#include "game/netplay/CanonicalDigestSink.h"
#include "game/netplay/CharacterProfile.h"
#include "game/screens/PlayScene.h"
#include "game/screens/PlayerPowerups.h"

namespace gdl::game {
namespace {
/** Explicit, fixed-width local schema. Never hash native padding, addresses,
 * process-local sound handles, profiling counters, or object representation. */
class ControlDigest {
public:
    void word(u32 value) {
        for (u32 shift = 0; shift < 32; shift += 8) {
            m_sink.push_back(static_cast<u8>(value >> shift));
        }
    }
    void wide(u64 value) {
        word(static_cast<u32>(value));
        word(static_cast<u32>(value >> 32));
    }
    void flag(bool value) { word(value ? 1U : 0U); }
    void integer(s32 value) { word(std::bit_cast<u32>(value)); }
    void real(f32 value) { word(std::bit_cast<u32>(value)); }
    void speech(const SpeechTiming& value) {
        wide(std::bit_cast<u64>(value.remainingSeconds));
        flag(value.looping);
    }
    void speech(std::span<const SpeechTiming> values) {
        wide(values.size());
        for (const auto& value : values) {
            speech(value);
        }
    }
    void vector(const Vec3& value) {
        real(value.x);
        real(value.y);
        real(value.z);
    }
    void string(std::string_view value) {
        wide(value.size());
        for (const char byte : value) {
            m_sink.push_back(static_cast<u8>(byte));
        }
    }
    void bytes(std::span<const u8> value) {
        wide(value.size());
        m_sink.append(value);
    }
    void camera(const WorldCamera& value) {
        vector(value.position);
        real(value.pitch);
        real(value.yaw);
        real(value.roll);
    }
    void camera(const std::optional<WorldCamera>& value) {
        flag(value.has_value());
        if (value) {
            camera(*value);
        }
    }
    void level(const LevelRef& value) {
        string(value.realm);
        integer(value.realmId);
        string(value.name);
        string(value.title);
        string(value.directory);
        string(value.items);
        string(value.ownItems);
        integer(value.index);
    }
    SimulationObservation::Digest finish() const { return m_sink.finish(); }

private:
    CanonicalDigestSink m_sink;
};

/** The live watchdog deliberately observes public native consequences, not a
 * resumable pool image. In particular, boss generators, unseen model bindings,
 * and native rosters beyond packet limits remain observable. Hidden RNG, queued
 * outputs, blast details beyond their count, retained inactive slots, and pose
 * caches are NOT covered by these projections. Equality is not proof that the
 * complete simulation state agrees; the test observer retains its separate,
 * explicitly partial diagnostic contract and checkpoint failure reporting. */
void nativeOpponents(ControlDigest& digest, const LevelOpponents& opponents) {
    digest.string("generators: public ordered native consequences");
    const auto& generators = opponents.generators();
    digest.wide(generators.count());
    for (usize index = 0; index < generators.count(); ++index) {
        const auto id = static_cast<s32>(index);
        digest.integer(generators.kindOf(id));
        digest.integer(generators.tierOf(id));
        digest.integer(generators.algorithmOf(id));
        digest.integer(generators.stateOf(id));
        digest.real(generators.healthOf(id));
        digest.integer(generators.mostOf(id));
        digest.integer(generators.intervalOf(id));
        digest.integer(generators.countdownOf(id));
        digest.integer(generators.bredOf(id));
        digest.integer(generators.livingOf(id));
        digest.vector(generators.positionOf(id));
        digest.integer(generators.floorOf(id));
        digest.flag(generators.standing(id));
        const auto& box = generators.boxOf(id);
        digest.vector(box.centre);
        digest.real(box.yaw);
        digest.real(box.halfAcross);
        digest.real(box.halfAlong);
        digest.real(box.height);
        digest.flag(box.solid);
        digest.real(box.cylinderRadius);
        digest.flag(box.enemyItem.has_value());
        if (box.enemyItem) {
            digest.flag(box.enemyItem->cylindrical);
            digest.real(box.enemyItem->radius);
        }
    }

    digest.string("swarm: public native slots and minds");
    const auto& enemies = opponents.enemies();
    digest.real(enemies.shrink());
    digest.integer(enemies.inView());
    for (s32 id = 0; id < Enemies::kMost; ++id) {
        const auto observation = enemies.observe(id);
        digest.flag(observation.has_value());
        if (!observation) {
            continue;
        }
        digest.wide(observation->instance);
        digest.real(observation->health);
        digest.real(observation->fullHealth);
        digest.flag(observation->asleep);
        digest.flag(enemies.alive(id));
        digest.flag(enemies.dying(id));
        digest.flag(enemies.draining(id));
        digest.integer(enemies.kindOf(id));
        digest.integer(enemies.tierOf(id));
        digest.integer(enemies.variantOf(id));
        digest.integer(enemies.algorithmOf(id));
        digest.integer(enemies.generatorOf(id));
        digest.flag(enemies.bred(id));
        digest.vector(enemies.positionOf(id));
        digest.real(enemies.yawOf(id));
        digest.real(enemies.radiusOf(id));
        digest.real(enemies.heightOf(id));
        digest.real(enemies.sightOf(id));
        digest.integer(enemies.floorOf(id));
        digest.integer(enemies.targetOf(id));
        digest.integer(enemies.stunTicksOf(id));
        digest.integer(enemies.pushCountOf(id));
        digest.flag(enemies.bumpedWallOf(id));
        digest.flag(enemies.blockedOf(id));
        const auto& mind = enemies.memoryOf(id);
        digest.integer(mind.effectiveWay);
        digest.real(mind.heading);
        digest.real(mind.headingBefore);
        digest.real(mind.seekHeading);
        digest.integer(mind.route);
        digest.integer(mind.collided);
        digest.integer(mind.stuck);
        digest.integer(mind.deadEnd);
        digest.integer(mind.turns);
        digest.integer(mind.counter);
        digest.integer(mind.mode);
        digest.integer(mind.fuse);
        digest.flag(mind.woken);
        digest.flag(mind.skirting);
        digest.flag(mind.keepingOff);
        digest.integer(mind.lookout);
        digest.flag(mind.primed);
        digest.integer(mind.zigZag.count);
        digest.integer(mind.zigZag.side);
        digest.integer(mind.zigZag.spread);
        digest.integer(mind.zigZag.hold);
        digest.integer(mind.zigZag.swings);
        const auto* animator = enemies.animatorOf(id);
        digest.flag(animator != nullptr);
        if (animator != nullptr) {
            digest.word(static_cast<u32>(animator->action()));
            digest.word(static_cast<u32>(animator->requested()));
            digest.real(animator->idleSeconds());
            digest.flag(animator->dead());
            digest.flag(animator->struck());
            digest.flag(animator->powerStruck());
            digest.flag(animator->threw());
            digest.word(animator->player().sequence());
            digest.real(animator->player().frame());
            digest.wide(animator->player().generation());
            digest.flag(animator->player().finished());
        }
    }

    digest.string("missiles: public native flights and blast count");
    const auto& missiles = opponents.missiles();
    digest.real(missiles.shrink());
    digest.wide(missiles.count());
    digest.wide(missiles.burstCount());
    for (usize index = 0; index < missiles.count(); ++index) {
        const auto& missile = missiles.missile(index);
        digest.wide(missile.instance);
        digest.word(missile.continuity);
        digest.vector(missile.position);
        digest.vector(missile.velocity);
        digest.vector(missile.turned);
        const auto& kind = missile.kind;
        digest.integer(kind.slot);
        digest.word(kind.flags);
        digest.real(kind.damage);
        digest.real(kind.speed);
        digest.real(kind.radius);
        digest.real(kind.burstRadius);
        digest.vector(kind.spin);
        digest.real(kind.weight);
        digest.real(kind.held);
        digest.flag(kind.throughWorld);
        digest.integer(missile.shooter);
        digest.real(missile.secondsLeft);
        digest.real(missile.lived);
        digest.real(missile.heldLeft);
        digest.real(missile.scale);
        digest.flag(missile.reflected);
        digest.wide(missile.pierced.size());
        for (const auto& [player, seconds] : missile.pierced) {
            digest.integer(player);
            digest.real(seconds);
        }
        // The model is borrowed artwork, not an identity or a flight parameter.
    }

    digest.string("placements: public ordered native roster and pending indices");
    const auto& placements = opponents.placements();
    digest.wide(placements.roster().size());
    for (const auto& placement : placements.roster()) {
        const auto& spawn = placement.spawn;
        digest.integer(spawn.zigZagSide);
        digest.integer(spawn.kind);
        digest.integer(spawn.tier);
        digest.integer(spawn.algorithm);
        digest.vector(spawn.position);
        digest.vector(spawn.direction);
        digest.real(spawn.clearance);
        digest.integer(spawn.generator);
        digest.flag(spawn.placed);
        digest.flag(spawn.patrolBirth);
        digest.flag(spawn.allBirthDirections);
        digest.flag(spawn.asleep);
        digest.real(spawn.throwInterval);
        digest.integer(static_cast<s32>(spawn.priority));
        digest.real(spawn.sight);
        digest.integer(placement.kind);
        digest.real(placement.facing);
        digest.real(placement.viewRadius);
        digest.real(placement.sight);
        digest.flag(placement.carried.has_value());
        if (placement.carried) {
            digest.wide(*placement.carried);
        }
    }
    digest.wide(placements.pending().size());
    for (const auto index : placements.pending()) {
        digest.wide(index);
    }

    digest.string("boss: public native encounter consequences");
    const auto& bosses = opponents.bosses();
    digest.flag(bosses.present());
    const auto boss = bosses.view();
    digest.integer(boss.kind);
    digest.string(boss.name);
    digest.real(boss.health);
    digest.real(boss.maxHealth);
    digest.flag(boss.awake);
    digest.flag(boss.alive);
    digest.flag(bosses.position() != nullptr);
    if (const auto* position = bosses.position()) {
        digest.vector(*position);
    }
    digest.real(bosses.facing());
    digest.string(bosses.moveName());
}
} // namespace

/** Read-only native follow-up hashing; no queues are consumed or restored. */
class SimulationObserver {
public:
    static SimulationObservation::Digest followups(const LevelOpponents& owner) {
        ControlDigest digest;
        digest.string("LevelOpponents native follow-up observation");
        digest.word(1);
        digest.wide(owner.m_clouds.size());
        for (const auto& cloud : owner.m_clouds) {
            digest.word(cloud.effect);
            digest.wide(cloud.stage);
            digest.vector(cloud.position);
        }
        digest.wide(owner.m_deathShots.size());
        for (const auto& shot : owner.m_deathShots) {
            digest.word(shot.burst);
            digest.integer(shot.kind);
            digest.vector(shot.position);
            digest.vector(shot.velocity);
            digest.flag(shot.flying);
        }
        for (const auto effect : owner.m_deathEffects) {
            digest.word(effect);
        }
        digest.flag(owner.m_deathContact);
        // m_yells/m_deathSound are backend-issued playback handles, not stable
        // native identities. They remain explicitly outside this observation.
        return digest.finish();
    }
};

SimulationObservation::Digest PlayScene::opponentFollowupDigest() const {
    return SimulationObserver::followups(m_opponents);
}

SimulationObservation::Digest PlayScene::simulationControlDigest() const {
    ControlDigest control;
    control.string("PlayScene scene_control partial observation");
    control.word(2); // Local field schema, not a protocol version.
    control.flag(m_open);
    control.flag(m_world != nullptr);
    if (m_world != nullptr) {
        control.level(m_world->ref());
    }
    control.word(simulationTickRate());
    control.real(m_playSeconds);
    control.real(m_towerNoticeSeconds);
    control.flag(m_leaving);
    control.flag(m_opponentsAdvanced);
    control.flag(m_projectilesAdvanced);
    control.flag(m_movementUpdated);
    control.level(m_destination);
    control.integer(m_refusedPortal);
    control.flag(m_secretTravel);
    control.vector(m_secretReturnPosition);
    control.flag(m_runeItem.has_value());
    if (m_runeItem) {
        control.wide(*m_runeItem);
    }
    control.flag(PlayerPowerups::timeStopped(m_players));
    control.flag(m_context.stopTimeTotal != nullptr);
    if (m_context.stopTimeTotal != nullptr) {
        control.real(*m_context.stopTimeTotal);
    }
    control.flag(m_arrival.active());
    control.real(m_arrival.titleScale());
    control.wide(m_arrival.effectCount());
    const auto& entrance = m_arrival.camera();
    control.word(static_cast<u32>(entrance.phase()));
    control.word(static_cast<u32>(entrance.mode()));
    control.integer(entrance.ticksLeft());
    control.camera(entrance.camera());
    control.vector(entrance.attention());
    control.word(static_cast<u32>(m_welcome.intro()));
    control.camera(m_welcome.camera());
    control.flag(m_sumnerVisit.active());
    control.integer(m_sumnerVisit.owner());
    control.word(m_sumnerVisit.menu().revision());
    control.flag(m_sumnerVisit.menu().reading());
    control.flag(m_sumnerVisit.menu().burning());
    control.flag(m_promotion.active());
    control.camera(m_promotion.camera());
    control.flag(m_towerRelics.active());
    control.word(static_cast<u32>(m_towerRelics.phase()));
    control.word(m_towerRelics.displayedRunes());
    control.word(m_towerRelics.displayedShards());
    control.word(m_towerRelics.pendingCeremonies());
    control.camera(m_towerRelics.camera());
    control.flag(m_messages.active());
    const auto message = m_messages.look();
    control.flag(message.has_value());
    if (message) {
        control.word(message->message);
        control.word(message->page);
        control.integer(message->burnFrame);
        control.word(message->promptAlpha);
    }
    control.flag(m_switchCutscene.active());
    control.flag(m_switchCutscene.showing());
    control.integer(m_switchCutscene.target());
    control.camera(m_switchCutscene.camera());
    control.flag(m_gameOver.active());
    control.flag(m_gameOver.finished());
    control.wide(m_gameOver.letters());
    control.word(static_cast<u32>(m_transition.phase()));
    control.real(m_transition.opacity());
    control.flag(m_departure.started());
    control.flag(m_departure.finished());
    control.real(m_departure.phase());
    control.word(static_cast<u32>(m_challenge.state()));
    control.real(m_challenge.remaining());
    control.real(m_challenge.duration());
    control.wide(m_challenge.coinsLeft());
    control.wide(m_challenge.totalCoins());
    control.integer(m_challenge.rewardClass());
    control.flag(m_dimmer.active());
    control.real(m_dimmer.offset());
    control.flag(m_shake.active());
    control.vector(m_shake.offset());
    control.word(static_cast<u32>(m_bossSequence.victory().state().stage()));
    control.speech(m_promotionVoice.timing);
    control.speech(m_relicVoice.timing);
    control.speech(m_bossSequence.speechTiming());
    control.speech(m_audio.narrationTiming());
    control.flag(m_audio.narrationHeld());
    control.speech(m_audio.barkTiming());
    return control.finish();
}

bool PlayScene::nativePresentationReady() const {
    if (!m_open || m_world == nullptr || !m_messages.nativeReady() ||
        (m_world->isTower() && !m_sumnerVisit.nativeReady(m_messages.text()))) {
        return false;
    }
    for (const auto& player : m_players) {
        // Offline loading permits missing art, but a native peer must have the
        // action clock which drives releases, reactions and death transitions.
        // Pojo and other intentional forms still retain this base animator.
        if (!player.departed && (player.figure == nullptr || !player.figure->animator().bound())) {
            return false;
        }
    }
    return true;
}

std::optional<SimulationObservation::Digest> PlayScene::nativeAgreementDigest() {
    if (!m_open || m_world == nullptr) {
        return std::nullopt;
    }
    ControlDigest digest;
    digest.string("PlayScene native agreement watchdog");
    digest.word(2);
    digest.bytes(simulationControlDigest());
    digest.wide(m_players.size());
    for (const auto& player : m_players) {
        const auto& actor = player.actor;
        const auto profile = CharacterProfile::capture(actor.save());
        const auto encoded = profile ? CharacterProfilePacket::encode(*profile) : std::nullopt;
        if (!encoded) {
            return std::nullopt;
        }
        digest.integer(actor.player());
        digest.bytes(*encoded);
        digest.vector(actor.position());
        digest.real(actor.yaw());
        digest.real(actor.speed());
        digest.real(actor.radius());
        digest.real(actor.height());
        digest.flag(actor.moving());
        digest.word(static_cast<u32>(player.life));
        digest.flag(player.departed);
        digest.flag(player.towerPrompt);
        digest.real(player.healthFraction);
        digest.integer(player.levelKills);
        digest.real(player.turbo.held());
        digest.integer(player.floor.object);
        digest.word(player.floor.flags);
        digest.word(static_cast<u32>(player.combo.role));
        digest.integer(player.combo.partner);
        digest.integer(player.combo.ticksLeft);
        digest.flag(player.capture.active());
        digest.integer(player.capture.owner());
        digest.integer(player.deathHeld);
        digest.integer(player.deathDrainTicks);
        digest.wide(player.helpHeard.size());
        for (const auto message : player.helpHeard) {
            digest.integer(message);
        }
    }
    nativeOpponents(digest, m_opponents);
    digest.bytes(opponentFollowupDigest());
    return digest.finish();
}

} // namespace gdl::game
