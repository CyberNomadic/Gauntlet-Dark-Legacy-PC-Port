#include "engine/core/Log.h"

#include "game/scenes/LegendsLevel.h"

namespace gdl::game {

void LegendsLevel::bossDefeated(const Vec3& position, std::span<PlayerActor*> players) {
    if (!m_boss || m_bossDefeated || players.empty()) {
        return;
    }
    m_bossDefeated = true;
    m_bossDepartureSeconds = 0;
    // These are native script grants, separate from the physical shard/armor
    // drops below and from every Dark Legacy campaign flag.
    for (auto* actor : players) {
        if (!actor) {
            continue;
        }
        auto& progress = actor->save().progress().legendsProgress(platform());
        if (m_boss->sharedObelisk >= 0) {
            progress.add(LegendsRelic::Obelisk, m_boss->sharedObelisk);
        }
        if (m_boss->clearsRunestones && m_boss->finalNativeGrant == 0) {
            progress.runestones = 0;
        }
    }
    m_world.adapter()->spawnBossRewards(position, m_playerStart);
    if (m_boss->finalNativeGrant != 0) {
        // Native 0x300290 is permanent, locked Anti-Death. GDL's timed,
        // toggleable inventory must not silently turn it into another reward.
        log::warn("Legends final victory: permanent Anti-Death reward is not yet supported; "
                  "retaining runestones");
    }
}

} // namespace gdl::game
