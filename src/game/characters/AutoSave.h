#pragma once

#include <span>

#include "engine/core/Types.h"

#include "game/characters/Party.h"

namespace gdl::game {
struct AutoSaveResult {
    usize written = 0;
    usize failed = 0;
    usize unavailable = 0; ///< no slot, invalid slot, or a slot claimed by two party members
};

/** Saves owned character slots in place and assigns each unsaved character a free slot
 * only after a successful write. Existing files, including corrupt saves, are never
 * chosen for a new character. The caller supplies only locally owned participants. */
AutoSaveResult autoSaveParty(SaveSlots& slots, std::span<PartyMember> party);
} // namespace gdl::game
