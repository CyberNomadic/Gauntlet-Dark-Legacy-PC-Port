#pragma once

#include <span>

#include "engine/core/Types.h"

namespace gdl::game {

struct PlayerRuntime;

/** Proves a help callback would do nothing using the complete native party's
 * checkpointed histories. New messages still require their HUD/audio owners. */
class PartyHelpAdmission {
public:
    /** PartyHud names a party-vector index, not an input slot. */
    static bool suppressed(s32 id, usize namedPartyIndex, std::span<const PlayerRuntime> party);
    /** LevelOpponents and PlayerHealth retain input-slot identities. */
    static bool suppressedForPlayer(s32 id, s32 inputSlot, std::span<const PlayerRuntime> party);
};

} // namespace gdl::game
