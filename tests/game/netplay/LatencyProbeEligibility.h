#pragma once

#include "game/netplay/CombatSnapshot.h"
#include "game/netplay/MatchSession.h"
#include "game/players/PlayerAnimator.h"

namespace gdl::test {
/** The headless probe observes submitted visuals, but only its authenticated
 * latest host checkpoint can certify that an input is currently eligible. */
struct LatencyProbeEligibility {
    enum class ReceiptEvidence { Ignore, Interrupted, Observe };
    /** Deadline belongs to the completed network receipt, not later replay or
     * rendering. This is action observation, never an execution/hit ACK. */
    static ReceiptEvidence receiptEvidence(const game::InputCommand& input, u32 continuity,
                                           game::MatchSession::Time pressed, u64 lastTick,
                                           const game::MatchSession::ActionReceipt& receipt) {
        if (receipt.seat != input.seat || receipt.receivedAt < pressed ||
            receipt.receivedAt - pressed > std::chrono::seconds(1)) {
            return ReceiptEvidence::Ignore;
        }
        if (receipt.epoch != input.epoch || !receipt.present || receipt.grant != input.grant ||
            receipt.continuity != continuity || receipt.life != game::ReplicaPlayerLife::Standing ||
            receipt.portalPhase) {
            return ReceiptEvidence::Interrupted;
        }
        return receipt.tick < input.tick || receipt.tick <= lastTick ? ReceiptEvidence::Ignore
                                                                     : ReceiptEvidence::Observe;
    }

    static bool standing(const game::CombatSnapshot* state, u8 seat) {
        return state != nullptr && seat < state->players.size() && state->players[seat] &&
               state->motion.players[seat] && state->motion.players[seat]->grant != 0 &&
               state->motion.players[seat]->continuity != 0 &&
               state->players[seat]->life == game::ReplicaPlayerLife::Standing &&
               !state->players[seat]->portalPhase;
    }

    static bool ready(const game::CombatSnapshot* shown, const game::CombatSnapshot* authority,
                      u64 epoch, u8 seat, u32 grant) {
        return epoch != 0 && grant != 0 && standing(shown, seat) && standing(authority, seat) &&
               shown->motion.epoch == epoch && authority->motion.epoch == epoch &&
               authority->players[seat]->movement.enabled &&
               authority->motion.players[seat]->grant == grant &&
               shown->motion.players[seat]->grant == grant &&
               shown->motion.players[seat]->continuity ==
                   authority->motion.players[seat]->continuity;
    }

    static bool visualIdle(const game::CombatSnapshot* shown, u8 seat) {
        return standing(shown, seat) &&
               shown->players[seat]->animation.action <=
                   static_cast<u32>(game::PlayerAnimator::Action::Idle2Loop);
    }

    static bool authorityIdle(const game::CombatSnapshot* authority, u8 seat) {
        if (!standing(authority, seat) || !authority->players[seat]->movement.enabled) {
            return false;
        }
        const auto& player = *authority->players[seat];
        const auto action =
            player.rollback ? static_cast<u32>(player.rollback->action) : player.animation.action;
        return action <= static_cast<u32>(game::PlayerAnimator::Action::Idle2Loop);
    }

    static bool idleForThrow(const game::CombatSnapshot* shown,
                             const game::CombatSnapshot* authority, u64 epoch, u8 seat, u32 grant) {
        return ready(shown, authority, epoch, seat, grant) && visualIdle(shown, seat) &&
               authorityIdle(authority, seat);
    }
};
} // namespace gdl::test
