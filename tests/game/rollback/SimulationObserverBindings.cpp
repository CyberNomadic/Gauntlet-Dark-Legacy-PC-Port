#include "engine/core/Log.h"

#include "game/rollback/SimulationObserver.h"
#include "game/scenes/StageResources.h"

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
    const auto checked = [](bool valid, std::string_view step) {
        if (!valid) {
            log::warn("Simulation observation binding refused: {}", step);
        }
        return valid;
    };
    if (!checked(m_scene.m_open && m_scene.m_device != nullptr && m_scene.m_world != nullptr,
                 "open scene") ||
        !checked(StageResources::preload(*m_scene.m_world, m_scene.m_opponents.enemies(),
                                         m_scene.m_opponents.critters(),
                                         m_scene.m_opponents.bosses()),
                 "stage resource preload")) {
        return false;
    }
    auto combat = std::make_unique<CombatResources>();
    if (!checked(combat->bindStage(*m_scene.m_device, *m_scene.m_world, m_scene.m_weapons,
                                   m_scene.m_classes, m_scene.m_players, m_scene.m_arsenal,
                                   m_scene.m_opponents, m_scene.m_effects),
                 "combat resource catalog")) {
        return false;
    }
    auto world = std::make_unique<LevelWorldCheckpoint>(*m_scene.m_world);
    auto fixtures = std::make_unique<LevelFixturesCheckpoint>(m_scene.m_fixtures);
    auto camera = std::make_unique<TowerCameraCheckpoint>(m_scene.m_camera, *m_scene.m_world);
    auto portals = std::make_unique<ExitPortalsCheckpoint>(
        m_scene.m_portals, m_scene.m_world->layout(), m_scene.m_world->items(),
        &m_scene.m_world->realmItems());
    auto transporters = std::make_unique<LevelTransportersCheckpoint>(m_scene.m_transporters);
    if (!checked(world->bindObservation(*m_scene.m_device), "world resources") ||
        !checked(fixtures->bindResources(), "fixture resources") ||
        !checked(portals->bindResources(), "portal resources") ||
        !checked(transporters->bindResources(), "transporter resources")) {
        return false;
    }
    auto worldState = world->observe();
    auto fixtureState = fixtures->checkpoint();
    auto cameraState = camera->checkpoint();
    auto portalState = portals->checkpoint();
    auto transporterState = transporters->checkpoint();
    if (!checked(worldState.has_value(), "world initial values") ||
        !checked(fixtureState.has_value(), "fixture initial values") ||
        !checked(cameraState.has_value(), "camera initial values") ||
        !checked(portalState.has_value(), "portal initial values") ||
        !checked(transporterState.has_value(), "transporter initial values")) {
        return false;
    }
    auto catalog = StageStatePacket::Catalog::buildObservation(
        {std::move(*worldState), std::move(*fixtureState), std::move(cameraState),
         std::move(portalState), std::move(transporterState)});
    if (!checked(catalog.has_value(), "stage immutable catalog")) {
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
