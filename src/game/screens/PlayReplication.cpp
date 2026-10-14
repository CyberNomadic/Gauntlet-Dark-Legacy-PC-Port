#include "game/screens/PlayReplication.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "game/screens/CombatCapture.h"
#include "game/screens/MatchInputs.h"
#include "game/screens/OpponentResources.h"
#include "game/screens/PlayerPowerups.h"
#include "game/screens/ReplicaHud.h"

namespace gdl::game {
bool PlayReplication::rosterMatches(const MatchContext& context) const {
    if (m_scene == nullptr || !m_scene->isOpen() ||
        m_scene->simulationTickRate() != context.tickRate) {
        return false;
    }
    std::array<bool, InputCommand::kSeats> seen{};
    for (const auto& runtime : m_scene->participants()) {
        const auto seat = runtime.actor.player();
        if (seat < 0 || seat >= static_cast<s32>(seen.size()) || seen[static_cast<usize>(seat)] ||
            runtime.departed || context.owners[static_cast<usize>(seat)] == 0) {
            return false;
        }
        seen[static_cast<usize>(seat)] = true;
    }
    for (usize seat = 0; seat < seen.size(); ++seat) {
        if (seen[seat] != (context.owners[seat] != 0)) {
            return false;
        }
    }
    return true;
}
bool PlayReplication::bind(PlayScene& scene, const MatchSession& match,
                           const PickupResources& pickups, const FixtureResources& fixtures) {
    if (!match.host() || match.phase() != MatchSession::Phase::Loading || match.tick() != 0 ||
        !match.context().valid() || match.context().epoch <= m_context.epoch) {
        return false;
    }
    PlayReplication candidate;
    candidate.m_scene = &scene;
    candidate.m_pickups = &pickups;
    candidate.m_fixtures = &fixtures;
    candidate.m_context = match.context();
    if (!candidate.rosterMatches(candidate.m_context) ||
        !candidate.m_fighters.bind(scene.critters(), scene.bosses())) {
        return false;
    }
    candidate.m_continuity.fill(1);
    for (usize i = 0; i < scene.missiles().count(); ++i) {
        candidate.m_lastMissile =
            std::max(candidate.m_lastMissile, scene.missiles().missile(i).instance);
    }
    for (const auto& history : scene.missiles().visualHistory()) {
        candidate.m_lastMissile = std::max(candidate.m_lastMissile, history.instance);
    }
    for (usize i = 0; i < scene.effects().count(); ++i) {
        candidate.m_lastEffect =
            std::max(candidate.m_lastEffect, scene.effects().effect(i).instance);
    }
    if (match.context().transition == MatchTransition::Resume && m_scene == &scene) {
        candidate.m_arrivalSkipVotes = m_arrivalSkipVotes;
    }
    *this = std::move(candidate);
    return true;
}
void PlayReplication::clear() {
    *this = PlayReplication{};
}
PlayReplication::Result PlayReplication::fail() {
    m_failed = true;
    if (m_scene != nullptr && !m_quiet) {
        m_scene->pauseGameplaySounds();
        m_quiet = true;
    }
    return Result::Failed;
}

std::optional<CombatPacket::Prepared>
PlayReplication::captureOpponents(CombatSnapshot& state) const {
    auto pools = OpponentResources::capture(m_scene->generators(), m_scene->enemies(),
                                            m_scene->enemyMissiles());
    if (!pools) {
        return std::nullopt;
    }
    pools->placements = m_scene->placedEnemies().checkpoint();
    if (!pools->placements) {
        return std::nullopt;
    }
    auto frame = m_scene->opponentFrame(static_cast<u32>(pools->enemies.scales.players));
    if (!frame) {
        return std::nullopt;
    }
    state.opponents = std::move(pools);
    state.opponentFrame = std::move(frame);
    // Existing snapshots retain their fixed raw budget. Optional simulation data
    // must never turn a valid presentation snapshot into a transport failure.
    // The caller only attempts this supplement on actual publication ticks.
    auto combat = m_scene->combatState();
    auto combatFrame = m_scene->combatFrame();
    if (combat && combatFrame) {
        state.combatState = std::move(combat);
        state.combatFrame = std::move(combatFrame);
        if (auto complete = CombatPacket::prepare(state)) {
            return complete;
        }
        // A full combat seed is atomic. Refusing it must not leave either half
        // attached to the ordinary opponent/presentation fallback.
        state.combatState.reset();
        state.combatFrame.reset();
    }
    // The complete encoder already validates all ordinary fields and includes
    // their bytes. Encode the smaller fallback only when it is actually needed.
    auto opponents = CombatPacket::prepare(state);
    if (!opponents) {
        state.opponents.reset();
        state.opponentFrame.reset();
    }
    return opponents;
}

std::optional<CombatSnapshot> PlayReplication::capture(u64 tick,
                                                       const ProjectileResources& resources) {
    MotionSnapshot motion;
    motion.epoch = m_context.epoch;
    motion.tick = tick;
    const auto increment = [](u32& continuity) {
        if (continuity == std::numeric_limits<u32>::max()) {
            return false;
        }
        ++continuity;
        return true;
    };
    if (m_latest && !m_scene->cameraContinuous() && !increment(m_cameraContinuity)) {
        return std::nullopt;
    }
    motion.cameraContinuity = m_cameraContinuity;
    motion.camera = m_scene->viewCamera();
    motion.camera.pitch = std::remainder(motion.camera.pitch, kTwoPi);
    motion.camera.yaw = std::remainder(motion.camera.yaw, kTwoPi);
    motion.camera.roll = std::remainder(motion.camera.roll, kTwoPi);
    const auto view = m_scene->cameraView();
    motion.horizontalFov = view.horizontalFov;
    motion.aspect = view.aspect;
    for (const auto& runtime : m_scene->participants()) {
        const auto seat = static_cast<usize>(runtime.actor.player());
        if (m_latest && !PartyFigures::presentationContinuous(runtime) &&
            !increment(m_continuity[seat])) {
            return std::nullopt;
        }
        motion.players[seat] =
            SeatMotion{m_context.grants[seat], m_continuity[seat], runtime.actor.position(),
                       std::remainder(runtime.actor.yaw(), kTwoPi)};
    }
    auto state = CombatCapture::capture(motion, m_scene->participants(), m_scene->enemies(),
                                        &m_scene->departure(), m_scene->world());
    const auto* world = m_scene->world();
    if (world == nullptr) {
        return std::nullopt;
    }
    const ProjectileCapture::Tower tower{&world->items(), &m_scene->sumner(),
                                         m_scene->spawning() ? nullptr : &m_scene->promotion(),
                                         &m_scene->towerRelics(), m_scene->showingRelicCeremony()};
    if (!state ||
        !ProjectileCapture::append(*state, resources, m_scene->missiles(), m_scene->enemyMissiles(),
                                   &m_scene->effects(), &m_scene->arrival(),
                                   world->isTower() ? &tower : nullptr)) {
        return std::nullopt;
    }
    state->geometry = world->scene().geometry();
    const auto& textures = world->textureAnimator();
    state->worldTextureFrame =
        static_cast<f32>(textures.frame()) + textures.presentationOffset(1).value_or(0);
    state->musicArea = m_scene->soundscape().musicArea();
    state->musicLevel = static_cast<u32>(m_scene->soundscape().musicLevel());
    for (usize seat = 0; seat < state->players.size(); ++seat) {
        auto& player = state->players[seat];
        if (player) {
            const auto speed = m_scene->predictionSpeed(static_cast<s32>(seat));
            if (speed) {
                player->movement.speed = *speed;
            }
            if (player->rollback) {
                player->modifiers = m_scene->weaponModifiers(static_cast<s32>(seat));
            }
            player->movement.current = player->movement.current && m_scene->movementPredictable();
            player->movement.enabled = player->movement.enabled && m_scene->movementPredictable();
            if (player->rollback && !PlayerAnimator::locomotion(player->rollback->action) &&
                !m_scene->itemsPredictable(static_cast<s32>(seat)) &&
                !(PlayerAnimator::rollbackAction(player->rollback->action) &&
                  m_scene->rangedItemsPredictable(static_cast<s32>(seat)))) {
                // Ordinary walking still uses the captured pace. An active guard
                // or attack may use unsupported item timing/poses; keep only its
                // current authoritative presentation until that action finishes.
                player->movement.enabled = false;
            }
            if (player->knockback && player->rollback) {
                player->knockbackTicks = m_scene->knockbackPreview(static_cast<s32>(seat));
                if (player->knockbackTicks != 0) {
                    player->movement.current = true;
                    player->movement.enabled = true;
                }
            }
            if (player->knockbackTicks == 0 && player->movement.current && player->rollback &&
                (PlayerAnimator::rollbackAction(player->rollback->action) ||
                 PlayerAnimator::rollbackMeleeAction(player->rollback->action) ||
                 PlayerAnimator::rollbackMovingMeleeAction(player->rollback->action))) {
                player->melee = m_scene->meleePreview(static_cast<s32>(seat));
                player->movement.enabled = player->movement.enabled || player->melee.has_value();
            }
            if (player->knockbackTicks == 0 && player->movement.current && player->rollback) {
                auto turbo = m_scene->turboPreview(static_cast<s32>(seat));
                auto region = turbo ? m_scene->turboGeometry(static_cast<s32>(seat)) : std::nullopt;
                if (turbo && region) {
                    player->shove = m_scene->shovePredictable(static_cast<s32>(seat), *region);
                    player->turbo = std::move(turbo);
                    player->turboGeometry = std::move(region);
                    player->movement.enabled = true;
                }
            }
            player->movement.ranged =
                player->knockbackTicks == 0 && player->movement.enabled && player->rollback &&
                !PlayerAnimator::rollbackTurboAction(player->rollback->action) &&
                !PlayerAnimator::rollbackShoveAction(player->rollback->action) &&
                !(player->melee && player->melee->moving) &&
                m_scene->rangedPredictable(static_cast<s32>(seat));
            if (!speed) {
                player->movement.enabled = false;
                player->movement.ranged = false;
                player->knockbackTicks = 0;
                player->melee.reset();
                player->turbo.reset();
                player->turboGeometry.reset();
                player->shove = false;
            }
            player->movement.cameraLimited =
                m_scene->bossCameraOn() ||
                std::ranges::count_if(state->players, [](const auto& member) {
                    return member && member->life == ReplicaPlayerLife::Standing;
                }) > 1;
            player->movement.cameraYaw = std::remainder(
                m_scene->bossCameraOn() ? m_scene->bossCamera().yaw() : m_scene->camera().yaw(),
                kTwoPi);
            player->movement.attention = m_scene->bossCameraOn() ? m_scene->bossCamera().attention()
                                                                 : m_scene->camera().attention();
        }
    }
    auto worldMotion = world->motion(PlayerPowerups::timeStopped(m_scene->participants()));
    const bool hasRider = std::ranges::any_of(state->players, [](const auto& player) {
        return player && player->movement.enabled && player->floor && player->floor->placement;
    });
    if (hasRider && m_scene->movementPredictable()) {
        bool certified = false;
        for (usize seat = 0; seat < state->players.size(); ++seat) {
            auto& player = state->players[seat];
            if (player && player->movement.enabled &&
                m_scene->platformPredictable(static_cast<s32>(seat), worldMotion,
                                             PlatformState::kMaxTicks)) {
                player->movement.platform = true;
                player->movement.ranged = false;
                player->melee.reset();
                player->turbo.reset();
                player->turboGeometry.reset();
                player->shove = false;
                player->knockbackTicks = 0;
                certified = true;
            }
        }
        if (certified) {
            state->worldMotionTicks = PlatformState::kMaxTicks;
        }
    }
    // Visible geometry and physical contact are independent native state. Even
    // without permission to advance a carrier, guests need exact collision flags.
    if (worldMotion.valid()) {
        state->worldMotion = std::move(worldMotion);
    }
    for (usize seat = 0; seat < state->players.size(); ++seat) {
        auto& player = state->players[seat];
        if (player && player->movement.current && !player->movement.platform &&
            player->knockbackTicks == 0) {
            auto contacts = m_scene->locomotionPreview(static_cast<s32>(seat));
            player->potion =
                contacts ? m_scene->potionPreview(static_cast<s32>(seat), *contacts) : std::nullopt;
            if (player->potion) {
                if (player->modifiers) {
                    player->potion->ticks =
                        std::min(player->potion->ticks, player->modifiers->ticks);
                }
                player->movement.enabled = true;
            }
            if (player->movement.enabled) {
                player->locomotion = std::move(contacts);
            }
            if (!player->locomotion && !player->turbo && !player->melee) {
                // Ordinary replay needs the native dynamic contacts too. An
                // overflow or unsupported floor is not an empty collision set.
                player->movement.enabled = false;
                player->movement.ranged = false;
            }
        }
        if (player && player->turbo) {
            player->turboEffects = m_scene->turboEffectPreview(static_cast<s32>(seat));
        }
    }
    state->hud = HudCapture::capture(m_scene->participants(), m_scene->hud(), m_scene->hudVisible(),
                                     &m_scene->bossMeter());
    if (!state->hud) {
        return std::nullopt;
    }
    using Ceremony = HudSnapshot::Ceremony;
    const auto& promotion = m_scene->promotion();
    if (!m_scene->spawning() && promotion.current() != nullptr) {
        const auto seat = m_scene->participants()[promotion.current()->player].actor.player();
        state->hud->ceremony = {Ceremony::Kind::Promotion, static_cast<u32>(seat), 0,
                                static_cast<u32>(std::min<usize>(promotion.shown(), 4096))};
    } else if (m_scene->showingRelicCeremony()) {
        const auto& relics = m_scene->towerRelics();
        if (relics.phase() == TowerRelics::Phase::Speech && relics.current() != nullptr) {
            const auto& entry = *relics.current();
            auto kind = Ceremony::Kind::Followup;
            if (entry.kind == TowerRelics::Kind::Rune) {
                kind = Ceremony::Kind::Rune;
            } else if (entry.kind == TowerRelics::Kind::Shard) {
                kind = Ceremony::Kind::Shard;
            }
            state->hud->ceremony = {
                kind, static_cast<u32>(entry.index), static_cast<u32>(relics.captionPage()),
                static_cast<u32>(std::min<usize>(relics.captionLetters(), 4096))};
        }
    }
    state->hud->screen = HudCapture::screen(m_scene->transition(), m_scene->arrival(),
                                            m_scene->gameOver(), m_scene->cinematicBars());
    if (const auto scroll = m_scene->scrollLook()) {
        state->hud->scroll =
            HudScroll{scroll->message, scroll->page, scroll->burnFrame, scroll->promptAlpha};
    }
    if (const auto hourglass = m_scene->hourglassLook()) {
        state->hud->hourglass = HudHourglass{hourglass->elapsed, hourglass->fallingFrame};
    }
    if (m_scene->runeMeter().visible()) {
        state->hud->runeFill = m_scene->runeMeter().fill();
    }
    if (world->level() == nullptr || world->level()->title.empty()) {
        state->hud->screen.titleScale = 0;
    }
    if (!FighterCapture::append(*state, m_fighters, m_scene->critters(), m_scene->bosses())) {
        return std::nullopt;
    }
    if (m_pickups == nullptr || !PickupCapture::append(*state, world->placedItems(), *m_pickups)) {
        return std::nullopt;
    }
    if (m_fixtures == nullptr ||
        !FixtureCapture::append(*state, *m_fixtures,
                                {m_scene->chests(), m_scene->gates(), world->triggers(),
                                 m_scene->generators(), m_scene->barrels(), m_scene->traps(),
                                 m_scene->safeRocks(), m_scene->rubble(), &m_scene->statues(),
                                 &m_scene->portals(), &m_scene->transporters()})) {
        return std::nullopt;
    }
    captureWeapons(*state, resources);
    captureShotOrigins(*state, resources);
    captureEffectOrigins(*state);
    return state->valid() ? std::move(state) : std::nullopt;
}

void PlayReplication::captureWeapons(CombatSnapshot& state, const ProjectileResources& resources) {
    for (const auto& runtime : m_scene->participants()) {
        const auto seat = static_cast<usize>(runtime.actor.player());
        auto& player = state.players[seat];
        const auto preview = m_scene->weaponPreview(static_cast<s32>(seat));
        // The resource/tuning descriptor identifies a supported native weapon;
        // it does not authorize an attack. A bounded melee query can select a
        // throw even when the all-bearing ranged certificate is unavailable.
        if (!player || !preview || runtime.figure == nullptr) {
            continue;
        }
        auto* archive = runtime.figure->missileArchive();
        if (archive == nullptr) {
            continue;
        }
        const auto tree = archive->trees.find(runtime.figure->missileTree());
        if (!tree) {
            continue;
        }
        PlayerWeaponState weapon;
        weapon.character = static_cast<u32>(preview->character);
        weapon.hand = preview->hand;
        weapon.speed = preview->speed;
        weapon.resource = resources.treeId(archive, &archive->trees.tree(*tree));
        const auto streak = m_scene->weaponStreak(runtime.actor);
        weapon.streak = resources.streakId(streak.texture);
        weapon.tint = streak.color;
        weapon.forward = streak.forward;
        weapon.target = preview->target;
        weapon.textureFrame = m_scene->weaponTextureFrame(*runtime.figure);
        weapon.shots = static_cast<u8>(preview->shots);
        if (player->modifiers && (player->modifiers->flags & 0xFu) != 0) {
            const auto rider = m_scene->weaponRider(runtime.actor, *runtime.figure);
            if (!rider) {
                continue;
            }
            weapon.rider = resources.treeId(rider->archive, rider->tree, rider->lenders);
            weapon.riderTextureFrame = rider->textureFrame;
            if (weapon.rider == 0) {
                continue;
            }
        }
        if (weapon.valid() && resources.acceptsWeapon(weapon)) {
            player->weapon = weapon;
        }
    }
}

void PlayReplication::captureEffectOrigins(CombatSnapshot& state) {
    std::map<u64, EffectIdentity> living;
    const auto& effects = m_scene->effects();
    for (usize i = 0; i < effects.count(); ++i) {
        const auto& effect = effects.effect(i);
        auto found = m_effectOrigins.find(effect.instance);
        if (found == m_effectOrigins.end() && effect.instance > m_lastEffect && effect.provenance) {
            const auto& provenance = *effect.provenance;
            if (provenance.player >= 0 &&
                provenance.player < static_cast<s32>(InputCommand::kSeats)) {
                const auto seat = static_cast<usize>(provenance.player);
                if (state.motion.players[seat]) {
                    const EffectIdentity origin{state.motion.epoch,      state.motion.tick,
                                                provenance.generation,   m_context.grants[seat],
                                                static_cast<u8>(seat),   provenance.occurrence,
                                                provenance.effectOrdinal};
                    if (origin.valid()) {
                        found = m_effectOrigins.emplace(effect.instance, origin).first;
                    }
                }
            }
        }
        if (found != m_effectOrigins.end()) {
            living.emplace(*found);
        }
    }
    // Capture runs after every native simulation step, independently of the
    // network snapshot stride. A retained tag therefore keeps its true birth
    // tick across later packets, morphs and temporary absence from the draw list.
    for (auto& shot : state.projectiles) {
        if (shot.source != ProjectileSource::WorldEffect) {
            continue;
        }
        const auto found = living.find(shot.instance);
        if (found != living.end()) {
            shot.effectOrigin = found->second;
        }
    }
    for (usize i = 0; i < effects.count(); ++i) {
        m_lastEffect = std::max(m_lastEffect, effects.effect(i).instance);
    }
    m_effectOrigins = std::move(living);
}

void PlayReplication::captureShotOrigins(CombatSnapshot& state,
                                         const ProjectileResources& resources) {
    const auto& missiles = m_scene->missiles();
    std::map<u64, ShotOrigin> living;
    std::map<u64, PotionIdentity> livingPotions;
    const auto admit = [&](u64 instance, s32 owner, const MissileRelease& release) {
        auto found = m_shotOrigins.find(instance);
        if (found == m_shotOrigins.end() && instance > m_lastMissile && owner >= 0 &&
            owner < static_cast<s32>(InputCommand::kSeats) && release.generation != 0) {
            const auto seat = static_cast<usize>(owner);
            if (state.motion.players[seat]) {
                const ShotIdentity identity{state.motion.epoch,    state.motion.tick,
                                            release.generation,    m_context.grants[seat],
                                            static_cast<u8>(seat), release.strong,
                                            release.ordinal,       release.emission};
                if (identity.valid()) {
                    found = m_shotOrigins
                                .emplace(instance,
                                         ShotOrigin{identity, static_cast<u32>(release.character)})
                                .first;
                }
            }
        }
        if (found != m_shotOrigins.end()) {
            living.emplace(*found);
        }
        return found;
    };
    const auto admitPotion = [&](u64 instance, s32 owner, const MissilePotionRelease& release) {
        auto found = m_potionOrigins.find(instance);
        if (found == m_potionOrigins.end() && instance > m_lastMissile && owner >= 0 &&
            owner < static_cast<s32>(InputCommand::kSeats) && release.generation != 0) {
            const auto seat = static_cast<usize>(owner);
            if (state.motion.players[seat]) {
                const PotionIdentity identity{state.motion.epoch,    state.motion.tick,
                                              release.generation,    m_context.grants[seat],
                                              static_cast<u8>(seat), PotionIdentity::Kind::Throw};
                if (identity.valid()) {
                    found = m_potionOrigins.emplace(instance, identity).first;
                }
            }
        }
        if (found != m_potionOrigins.end()) {
            livingPotions.emplace(*found);
        }
        return found;
    };
    // The physical missile can disappear during its birth tick. Native particle
    // history survives that impact and must keep the same release identity until
    // its last emitter has finished, including across later checkpoints.
    for (const auto& history : missiles.visualHistory()) {
        if (history.release) {
            admit(history.instance, history.owner, *history.release);
        }
        if (history.potion >= 1 && history.potion <= 4 && history.potionRelease) {
            admitPotion(history.instance, history.owner, *history.potionRelease);
        }
    }
    std::map<u32, u64> effectInstances;
    for (usize i = 0; i < missiles.visuals().count(); ++i) {
        const auto& effect = missiles.visuals().effect(i);
        effectInstances.emplace(effect.id, effect.instance);
    }
    std::map<std::pair<ProjectileSource, u64>, ProjectileState*> components;
    for (auto& shot : state.projectiles) {
        if (shot.particles) {
            shot.particles->held = !m_scene->projectilesAdvanced();
        }
        components.emplace(shot.key(), &shot);
    }
    const auto component = [&](ProjectileSource source, u64 instance) -> ProjectileState* {
        const auto found = components.find({source, instance});
        return found != components.end() ? found->second : nullptr;
    };
    const auto effectComponent = [&](ProjectileSource source, u32 effect) -> ProjectileState* {
        const auto found = effectInstances.find(effect);
        return found != effectInstances.end() ? component(source, found->second) : nullptr;
    };
    for (usize i = 0; i < missiles.count(); ++i) {
        const auto& missile = missiles.missile(i);
        auto origin = m_shotOrigins.find(missile.instance);
        if (origin == m_shotOrigins.end() && missile.instance > m_lastMissile &&
            missile.owner >= 0 && missile.owner < static_cast<s32>(InputCommand::kSeats) &&
            missile.potion == 0 && PlayerMissiles::replayModifiers(missile.flags) &&
            missile.breaksPotions && missile.multiplayer == MultiplayerMode::Normal &&
            missile.release) {
            origin = admit(missile.instance, missile.owner, *missile.release);
        }
        if (origin == m_shotOrigins.end()) {
            continue;
        }
        living.emplace(*origin);
        for (auto* shot : {component(ProjectileSource::Player, missile.instance),
                           component(ProjectileSource::Streak, missile.instance),
                           effectComponent(ProjectileSource::PlayerEffect, missile.effect),
                           effectComponent(ProjectileSource::PlayerRider, missile.rider)}) {
            if (shot != nullptr) {
                shot->rollback = ProjectileState::Rollback{
                    origin->second.identity, origin->second.character,
                    PlayerMissiles::flightCheckpoint(missile), !m_scene->projectilesAdvanced()};
            }
        }
    }
    for (usize i = 0; i < missiles.count(); ++i) {
        const auto& missile = missiles.missile(i);
        if (missile.potion < 1 || missile.potion > 4 || !missile.potionRelease ||
            missile.potionRelease->generation == 0 || missile.owner < 0 ||
            missile.owner >= static_cast<s32>(InputCommand::kSeats)) {
            continue;
        }
        auto* shot = effectComponent(ProjectileSource::PlayerEffect, missile.effect);
        if (shot == nullptr || shot->rollback || shot->effectOrigin ||
            (resources.hasParticles(shot->resource) &&
             (!shot->particles || shot->particles->potion != missile.potion))) {
            continue;
        }
        const auto origin = admitPotion(missile.instance, missile.owner, *missile.potionRelease);
        if (origin != m_potionOrigins.end()) {
            ProjectileState::PotionRollback value{origin->second, static_cast<u8>(missile.potion),
                                                  PlayerMissiles::flightCheckpoint(missile),
                                                  !m_scene->projectilesAdvanced()};
            if (value.valid()) {
                shot->potion = value;
            }
        }
    }
    for (usize i = 0; i < missiles.count(); ++i) {
        m_lastMissile = std::max(m_lastMissile, missiles.missile(i).instance);
    }
    for (const auto& history : missiles.visualHistory()) {
        m_lastMissile = std::max(m_lastMissile, history.instance);
        const auto potionOrigin = livingPotions.find(history.instance);
        if (potionOrigin != livingPotions.end()) {
            auto* shot = effectComponent(ProjectileSource::PlayerEffect, history.effect);
            if (shot != nullptr && shot->particles && shot->particles->potion == history.potion) {
                shot->particles->potionOrigin = potionOrigin->second;
            }
        }
        const auto origin = living.find(history.instance);
        if (origin == living.end()) {
            continue;
        }
        for (auto* shot : {effectComponent(ProjectileSource::PlayerEffect, history.effect),
                           effectComponent(ProjectileSource::PlayerRider, history.rider)}) {
            if (shot != nullptr && shot->particles) {
                shot->particles->origin = origin->second.identity;
            }
        }
    }
    m_potionOrigins = std::move(livingPotions);
    m_shotOrigins = std::move(living);
}
void PlayReplication::filterArrivalSkip(SessionInputs::Frame& inputs) {
    const auto& camera = m_scene->arrival().camera();
    if (!camera.active()) {
        m_arrivalSkipVotes = {};
        return;
    }
    // Only legacy boss-entry holds are skippable. Keep the native eligibility
    // window, but require the whole online party instead of its first button.
    const bool eligible = camera.mode() == StartCamera::Mode::Legacy &&
                          camera.phase() == StartCamera::Phase::Hold &&
                          camera.ticksLeft() <= StartCamera::kSkipBelow;
    bool unanimous = eligible;
    for (usize seat = 0; seat < inputs.size(); ++seat) {
        auto& menu = inputs[seat].menu;
        if (eligible && m_context.owners[seat] != 0) {
            m_arrivalSkipVotes[seat] =
                m_arrivalSkipVotes[seat] || menu.start || menu.select || menu.back;
            unanimous = unanimous && m_arrivalSkipVotes[seat];
        }
        menu.start = false;
        menu.select = false;
        menu.back = false;
    }
    if (unanimous) {
        for (usize seat = 0; seat < inputs.size(); ++seat) {
            inputs[seat].menu.select = m_context.owners[seat] != 0;
        }
    }
}

PlayReplication::Result PlayReplication::advance(MatchSession& match,
                                                 const SessionInputs::Frame& local,
                                                 const ProjectileResources& resources) {
    if (m_failed) {
        return Result::Failed;
    }
    if (m_scene == nullptr || m_finished) {
        return Result::Held;
    }
    if (!match.host() || match.context() != m_context || match.tick() != m_nextTick ||
        !rosterMatches(m_context)) {
        return fail();
    }
    if (match.phase() != MatchSession::Phase::Running) {
        if (!m_quiet) {
            m_scene->pauseGameplaySounds();
            m_quiet = true;
        }
        return Result::Held;
    }
    if (!MatchInputs::sample(match, local)) {
        return fail();
    }
    auto inputs = MatchInputs::advance(match);
    if (!inputs) {
        return fail();
    }
    filterArrivalSkip(*inputs);
    m_quiet = false;
    const auto outcome = m_scene->update(1.0 / m_context.tickRate, *inputs);
    ++m_nextTick;
    if (!rosterMatches(m_context)) {
        // Mid-match departure needs a new admitted roster, not a silent slot edit.
        return fail();
    }
    auto state = capture(match.tick() - 1, resources);
    if (state) {
        for (usize seat = 0; seat < state->players.size(); ++seat) {
            if (auto& player = state->players[seat]) {
                const auto& diagnostics = match.inputDiagnostics(seat);
                auto& movement = player->movement;
                movement.receivedInput = diagnostics.received.value_or(0);
                movement.arrivalMargin = diagnostics.arrivalMargin;
                movement.lateInputs = diagnostics.late;
                player->rollbackInput = match.resolvedInput(seat);
            }
        }
    }
    if (!state) {
        return fail();
    }
    if ((state->motion.tick % kSnapshotStride == 0 || outcome != PlayOutcome::Running) &&
        match.publicationReady()) {
        const auto prepared = captureOpponents(*state);
        if (!(prepared ? match.publish(*prepared) : match.publish(*state))) {
            return fail();
        }
    }
    m_latest = std::move(state);
    m_finished = outcome != PlayOutcome::Running;
    switch (outcome) {
    case PlayOutcome::Running: return Result::Advanced;
    case PlayOutcome::Travel: return Result::Travel;
    case PlayOutcome::GameOver: return Result::GameOver;
    case PlayOutcome::Leave: return Result::Leave;
    }
    return fail();
}
} // namespace gdl::game
