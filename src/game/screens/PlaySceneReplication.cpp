#include <algorithm>
#include <chrono>

#include "game/screens/PlayScene.h"
#include "game/screens/PlayerPowerups.h"
#include "game/screens/ReplicaFixtures.h"
#include "game/screens/ReplicaPickups.h"
#include "game/screens/ReplicaProjectiles.h"
#include "game/screens/StageResources.h"

namespace gdl::game {
namespace {
class MeasureCapture {
public:
    explicit MeasureCapture(PlayScene::CombatCaptureCosts::Cost& cost)
        : m_cost(&cost), m_begin(std::chrono::steady_clock::now()) {}
    ~MeasureCapture() { finish(); }
    void finish() {
        if (m_cost == nullptr) {
            return;
        }
        const auto elapsed = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                  std::chrono::steady_clock::now() - m_begin)
                                                  .count());
        ++m_cost->count;
        m_cost->nanoseconds += elapsed;
        m_cost->maximumNanoseconds = std::max(m_cost->maximumNanoseconds, elapsed);
        m_cost = nullptr;
    }

private:
    PlayScene::CombatCaptureCosts::Cost* m_cost;
    std::chrono::steady_clock::time_point m_begin;
};
} // namespace
std::optional<CombatStatePacket::State> PlayScene::combatState() {
    const MeasureCapture totalCost(m_combatCaptureCosts.total);
    if (!combatFrame() || !m_combatResources || !m_combatResources->matches(m_players)) {
        return std::nullopt;
    }
    MeasureCapture captureCost(m_combatCaptureCosts.capture);
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
    OpponentCombatCheckpoint adapter{m_opponents, m_combatResources->resources().sharedEffects};
    auto opponents = adapter.checkpoint(combat->effects);
    if (!opponents) {
        return std::nullopt;
    }
    CombatStatePacket::State state{std::move(*combat), std::move(*opponents)};
    if (m_combatWorld || m_combatFixtures) {
        if (!m_combatWorld || !m_combatFixtures || !m_combatCamera || !m_combatPortals ||
            !m_combatTransporters || !m_combatTransporters->binds(m_transporters) ||
            !m_combatWorld->binds(*m_world) || !m_combatFixtures->binds(m_fixtures) ||
            !m_combatPortals->binds(m_portals) || !m_combatCamera->binds(m_camera, *m_world)) {
            return std::nullopt;
        }
        auto world = m_combatWorld->checkpoint();
        auto fixtures = m_combatFixtures->checkpoint();
        auto camera = m_combatCamera->checkpoint();
        auto portals = m_combatPortals->checkpoint();
        auto transporters = m_combatTransporters->checkpoint();
        if (!world || !fixtures || !camera || !portals || !transporters) {
            return std::nullopt;
        }
        captureCost.finish();
        const MeasureCapture encodeCost(m_combatCaptureCosts.encode);
        state.stage =
            StageStatePacket::encode({std::move(*world), std::move(*fixtures), std::move(*camera),
                                      std::move(*portals), std::move(*transporters)},
                                     m_combatStageCatalog);
        if (!state.stage) {
            return std::nullopt;
        }
    }
    captureCost.finish();
    if (!state.valid()) {
        return std::nullopt;
    }
    ++m_combatCaptureCosts.accepted;
    return state;
}

std::optional<OrdinaryCombatTick::Frame> PlayScene::combatFrame() const {
    // The complete native phase must actually have run. Welcome crystal cuts
    // still advance opponents while holding PartyMotion, so neither opponent
    // clocks nor the camera alone establish a replayable boundary.
    if (!movementPredictable() || !m_opponentsAdvanced || m_world == nullptr ||
        simulationTickRate() != 60 || PlayerPowerups::timeStopped(m_players) || m_dimmer.active()) {
        return std::nullopt;
    }
    // Pending tower notices/visits and secret-level deadlines can interrupt
    // the next phase even while the current frame is freely moving.
    if (m_world->isTower() || m_world->ref().isSecret()) {
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
    frame.autoMelee = m_context.config != nullptr && m_context.config->combat.autoMelee;
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
    m_combatFixtures.reset();
    m_combatPortals.reset();
    m_combatTransporters.reset();
    m_combatCamera.reset();
    m_combatWorld.reset();
    m_combatStageCatalog = {};
    auto combat = std::make_unique<CombatResources>();
    if (combat->bindStage(*m_device, *m_world, m_weapons, m_classes, m_players, m_arsenal,
                          m_opponents, m_effects)) {
        m_combatResources = std::move(combat);
        auto world = std::make_unique<LevelWorldCheckpoint>(*m_world);
        auto fixtureOwner = std::make_unique<LevelFixturesCheckpoint>(m_fixtures);
        auto cameraOwner = std::make_unique<TowerCameraCheckpoint>(m_camera, *m_world);
        auto portalOwner = std::make_unique<ExitPortalsCheckpoint>(
            m_portals, m_world->layout(), m_world->items(), &m_world->realmItems());
        auto transporterOwner = std::make_unique<LevelTransportersCheckpoint>(m_transporters);
        if (world->bindResources(*m_device) && fixtureOwner->bindResources() &&
            portalOwner->bindResources() && transporterOwner->bindResources()) {
            auto worldState = world->checkpoint();
            auto fixtureState = fixtureOwner->checkpoint();
            auto cameraState = cameraOwner->checkpoint();
            auto portalState = portalOwner->checkpoint();
            auto transporterState = transporterOwner->checkpoint();
            if (worldState && fixtureState && cameraState && portalState && transporterState) {
                auto catalog = StageStatePacket::Catalog::build(
                    {std::move(*worldState), std::move(*fixtureState), std::move(*cameraState),
                     std::move(*portalState), std::move(*transporterState)});
                if (catalog) {
                    m_combatStageCatalog = std::move(*catalog);
                    m_combatWorld = std::move(world);
                    m_combatFixtures = std::move(fixtureOwner);
                    m_combatCamera = std::move(cameraOwner);
                    m_combatPortals = std::move(portalOwner);
                    m_combatTransporters = std::move(transporterOwner);
                }
            }
        }
    } else {
        m_combatResources.reset();
    }
    return true;
}
} // namespace gdl::game
