#include "game/screens/PlayScene.h"
#include "game/screens/PlayerPowerups.h"
#include "game/screens/ReplicaFixtures.h"
#include "game/screens/ReplicaPickups.h"
#include "game/screens/ReplicaProjectiles.h"
#include "game/screens/StageResources.h"

namespace gdl::game {
std::optional<CombatStatePacket::State> PlayScene::combatState() {
    if (!combatFrame() || !m_combatResources || !m_combatResources->matches(m_players)) {
        return std::nullopt;
    }
    std::vector<std::unique_ptr<PlayerRuntimeCheckpoint>> owners;
    std::vector<PlayerRuntimeCheckpoint*> players;
    owners.reserve(m_players.size());
    players.reserve(m_players.size());
    for (auto& player : m_players) {
        owners.push_back(std::make_unique<PlayerRuntimeCheckpoint>(
            player, m_classes.stats(player.actor.save().character)));
        players.push_back(owners.back().get());
    }
    CombatRollbackCheckpoint capture{players,
                                     m_health,
                                     m_levels,
                                     m_attacks,
                                     m_arsenal,
                                     m_effects,
                                     m_combatResources->resources()};
    auto combat = capture.checkpoint();
    if (!combat) {
        return std::nullopt;
    }
    OpponentCombatCheckpoint adapter{m_opponents};
    auto opponents = adapter.checkpoint(combat->effects);
    if (!opponents) {
        return std::nullopt;
    }
    CombatStatePacket::State state{std::move(*combat), std::move(*opponents)};
    return state.valid() ? std::optional{std::move(state)} : std::nullopt;
}

std::optional<OrdinaryCombatTick::Frame> PlayScene::combatFrame() const {
    // The complete native phase must actually have run. Welcome crystal cuts
    // still advance opponents while holding PartyMotion, so neither opponent
    // clocks nor the camera alone establish a replayable boundary.
    if (!movementPredictable() || !m_opponentsAdvanced || m_world == nullptr ||
        simulationTickRate() != 60 || PlayerPowerups::timeStopped(m_players)) {
        return std::nullopt;
    }
    OrdinaryCombatTick::Frame frame;
    frame.camera = bossCameraOn() ? m_bossCamera.camera() : m_camera.camera();
    frame.projection = cameraView();
    frame.attention = bossCameraOn() ? m_bossCamera.attention() : m_camera.attention();
    const auto* level = m_world->level();
    frame.damage.tower = m_world->isTower();
    frame.damage.damageScale = level != nullptr ? level->tuning.damage : 1;
    frame.damage.bossEncounter = level != nullptr && level->bossType >= 0;
    frame.damage.cutscene = m_switchCutscene.active();
    frame.damage.lastRealmLevel =
        m_context.levels != nullptr && m_context.levels->isLastLevel(m_world->ref());
    frame.damage.realm = m_world->ref().realmId;
    // Capture is post-step; the next native phase adds its own seconds once.
    frame.damage.playSeconds = m_playSeconds;
    return frame.valid() ? std::optional{frame} : std::nullopt;
}

bool PlayScene::bindPlayerProjectiles(ProjectileResources& resources) {
    if (!m_open || m_device == nullptr) {
        return false;
    }
    std::array<PlayerFigure*, InputCommand::kSeats> figures{};
    for (const auto& player : m_players) {
        const auto seat = player.actor.player();
        if (seat < 0 || static_cast<usize>(seat) >= figures.size() || player.figure == nullptr ||
            figures[static_cast<usize>(seat)] != nullptr) {
            return false;
        }
        figures[static_cast<usize>(seat)] = player.figure.get();
    }
    return resources.addPlayers(*m_device, m_weapons, figures, m_effects.textureLenders());
}
bool PlayScene::bindSceneProjectiles(ProjectileResources& resources) {
    if (!m_open || m_device == nullptr || m_world == nullptr) {
        return false;
    }
    ProjectileResources next = resources;
    auto stocks = m_opponents.critters().resources();
    for (auto* stock : m_opponents.bosses().resources()) {
        stocks.push_back(stock);
    }
    const std::array archives{&m_world->items(), &m_world->realmItems(), &m_world->powerups()};
    if (!bindPlayerProjectiles(next) ||
        !next.addEnemies(*m_device, m_opponents.enemies(), m_effects.textureLenders()) ||
        !next.addStage(*m_device, archives, stocks, m_effects.textureLenders())) {
        return false;
    }
    resources = std::move(next);
    return true;
}
bool PlayScene::bindReplicationResources(ProjectileResources& projectiles, PickupResources& pickups,
                                         FixtureResources& fixtures) {
    if (!m_open || m_device == nullptr || m_world == nullptr ||
        !StageResources::preload(*m_world, m_opponents.enemies(), m_opponents.critters(),
                                 m_opponents.bosses())) {
        return false;
    }
    ProjectileResources nextProjectiles;
    PickupResources nextPickups;
    FixtureResources nextFixtures;
    const auto archives = StageResources::fixtureArchives(
        *m_world, m_opponents.enemies(), m_opponents.critters(), m_opponents.bosses());
    if (!bindSceneProjectiles(nextProjectiles) ||
        !nextPickups.bind(*m_device, m_world->placedItems().archives()) ||
        !nextFixtures.bind(*m_device, archives, m_opponents.generators(), m_fixtures.safeRocks())) {
        return false;
    }
    projectiles = std::move(nextProjectiles);
    pickups = std::move(nextPickups);
    fixtures = std::move(nextFixtures);
    // Optional complete combat replay never loads or uploads resources while
    // capturing a tick. Unsupported artwork retains ordinary online playback.
    auto combat = std::make_unique<CombatResources>();
    if (combat->bindStage(*m_device, *m_world, m_weapons, m_classes, m_players, m_arsenal,
                          m_opponents, m_effects)) {
        m_combatResources = std::move(combat);
    } else {
        m_combatResources.reset();
    }
    return true;
}
} // namespace gdl::game
