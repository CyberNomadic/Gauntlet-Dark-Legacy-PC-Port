#include "game/screens/SimulationObserver.h"
#include "game/screens/StageResources.h"

namespace gdl::game {

void SimulationObserver::clear() {
    m_combatFixtures.reset();
    m_combatPortals.reset();
    m_combatTransporters.reset();
    m_combatCamera.reset();
    m_combatWorld.reset();
    m_combatStageCatalog = {};
    m_combatResources.reset();
    m_generation = m_scene.m_openGeneration;
}

bool SimulationObserver::bind() {
    clear();
    if (!m_scene.m_open || m_scene.m_device == nullptr || m_scene.m_world == nullptr ||
        !StageResources::preload(*m_scene.m_world, m_scene.m_opponents.enemies(),
                                 m_scene.m_opponents.critters(), m_scene.m_opponents.bosses())) {
        return false;
    }
    auto combat = std::make_unique<CombatResources>();
    if (!combat->bindStage(*m_scene.m_device, *m_scene.m_world, m_scene.m_weapons,
                           m_scene.m_classes, m_scene.m_players, m_scene.m_arsenal,
                           m_scene.m_opponents, m_scene.m_effects)) {
        return false;
    }
    auto world = std::make_unique<LevelWorldCheckpoint>(*m_scene.m_world);
    auto fixtures = std::make_unique<LevelFixturesCheckpoint>(m_scene.m_fixtures);
    auto camera = std::make_unique<TowerCameraCheckpoint>(m_scene.m_camera, *m_scene.m_world);
    auto portals = std::make_unique<ExitPortalsCheckpoint>(
        m_scene.m_portals, m_scene.m_world->layout(), m_scene.m_world->items(),
        &m_scene.m_world->realmItems());
    auto transporters = std::make_unique<LevelTransportersCheckpoint>(m_scene.m_transporters);
    if (!world->bindResources(*m_scene.m_device) || !fixtures->bindResources() ||
        !portals->bindResources() || !transporters->bindResources()) {
        return false;
    }
    auto worldState = world->checkpoint();
    auto fixtureState = fixtures->checkpoint();
    auto cameraState = camera->checkpoint();
    auto portalState = portals->checkpoint();
    auto transporterState = transporters->checkpoint();
    if (!worldState || !fixtureState || !cameraState || !portalState || !transporterState) {
        return false;
    }
    auto catalog = StageStatePacket::Catalog::build(
        {std::move(*worldState), std::move(*fixtureState), std::move(*cameraState),
         std::move(*portalState), std::move(*transporterState)});
    if (!catalog) {
        return false;
    }
    m_combatResources = std::move(combat);
    m_combatStageCatalog = std::move(*catalog);
    m_combatWorld = std::move(world);
    m_combatFixtures = std::move(fixtures);
    m_combatCamera = std::move(camera);
    m_combatPortals = std::move(portals);
    m_combatTransporters = std::move(transporters);
    return true;
}
} // namespace gdl::game
