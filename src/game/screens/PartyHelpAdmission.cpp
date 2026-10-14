#include "game/screens/PartyHelpAdmission.h"

#include <array>

#include "game/screens/HelpMessages.h"
#include "game/screens/PlayerRuntime.h"

namespace gdl::game {

bool PartyHelpAdmission::suppressed(s32 id, usize namedPartyIndex,
                                    std::span<const PlayerRuntime> party) {
    if (namedPartyIndex >= party.size()) {
        return true;
    }
    std::array<HelpHistory, 4> readers;
    if (party.size() > readers.size()) {
        return false;
    }
    usize count = 0;
    for (const auto& player : party) {
        // Match PartyHud::postHelp exactly, including its life-only filter.
        if (player.life == PlayerLife::Standing) {
            readers[count++] = {player.actor.player(), &player.actor.save().helpSeen,
                                &player.helpHeard};
        }
    }
    return !HelpMessages::wanted(id, party[namedPartyIndex].actor.player(),
                                 std::span<const HelpHistory>{readers.data(), count});
}

bool PartyHelpAdmission::suppressedForPlayer(s32 id, s32 inputSlot,
                                             std::span<const PlayerRuntime> party) {
    for (usize index = 0; index < party.size(); ++index) {
        if (party[index].actor.player() == inputSlot && !suppressed(id, index, party)) {
            return false;
        }
    }
    // Native slot routing makes no help call when its recipient is absent.
    return true;
}

} // namespace gdl::game
