#pragma once

#include <catch2/catch_test_macros.hpp>

#include "game/netplay/CombatSnapshot.h"

namespace gdl::test {
/** No assets are needed to exercise the complete owner envelope. Native actor
 * spawn/checkpoint supplies valid state instead of hand-assembling its internals. */
inline void combatSeed(game::CombatSnapshot& snapshot) {
    using namespace game;
    snapshot.opponents.emplace();
    snapshot.opponents->placements.emplace();
    snapshot.opponentFrame.emplace();
    snapshot.combatFrame.emplace();
    snapshot.combatFrame->camera.position = {17, 23, 41};
    snapshot.combatFrame->camera.yaw = 0.25f;
    snapshot.combatFrame->attention = {3, 5, 7};
    snapshot.combatFrame->damage.realm = 1;
    snapshot.combatFrame->damage.playSeconds = 13.25f;
    auto& seed = snapshot.combatState.emplace();
    seed.combat.health = Mt19937{0x12345678}.checkpoint();
    ClassStats stats;
    for (usize seat = 0; seat < snapshot.players.size(); ++seat) {
        if (!snapshot.players[seat]) {
            continue;
        }
        REQUIRE(snapshot.motion.players[seat]);
        const auto& motion = *snapshot.motion.players[seat];
        PlayerRuntime runtime;
        CharacterSave save;
        save.name = "TEST";
        save.color = static_cast<s32>(seat);
        save.progress().health = 1000;
        runtime.actor.spawn(static_cast<s32>(seat), save, &stats, motion.position, motion.yaw);
        runtime.entrySave = save;
        runtime.healthFraction = snapshot.players[seat]->health - 1000;
        PlayerRuntimeCheckpoint owner{runtime, &stats};
        auto player = owner.checkpoint();
        REQUIRE(player);
        seed.combat.attacks.actors.push_back({player->player, player->save.character});
        seed.combat.players.push_back(std::move(*player));
        EnemyView view;
        view.player = static_cast<s32>(seat);
        view.position = motion.position;
        snapshot.opponentFrame->players.push_back({static_cast<u8>(seat), view});
    }
    REQUIRE(snapshot.valid());
}
} // namespace gdl::test
