#pragma once

#include "game/enemies/CombatResources.h"
#include "game/rollback/CombatStatePacket.h"
#include "game/rollback/StageStatePacket.h"
#include "game/scenes/PlayScene.h"

namespace gdl::game {

/** Test-only read-only observations of native owners. The scene must outlive
 * this observer. Closing/reopening a scene invalidates all retained bindings;
 * bind again to observe a new lifetime. Never runs a replay or restores state. */
class SimulationObserver {
public:
    explicit SimulationObserver(PlayScene& scene) : m_scene(scene) {}
    bool bind();
    SimulationObservation observe();

private:
    void clear();
    PlayScene& m_scene;
    u64 m_generation = 0;
    std::unique_ptr<CombatResources> m_combatResources;
    std::unique_ptr<LevelWorldCheckpoint> m_combatWorld;
    std::unique_ptr<LevelFixturesCheckpoint> m_combatFixtures;
    std::unique_ptr<TowerCameraCheckpoint> m_combatCamera;
    std::unique_ptr<ExitPortalsCheckpoint> m_combatPortals;
    std::unique_ptr<LevelTransportersCheckpoint> m_combatTransporters;
    StageStatePacket::Catalog m_combatStageCatalog;
};
} // namespace gdl::game
