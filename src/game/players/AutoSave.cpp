#include "game/players/AutoSave.h"

#include <algorithm>

namespace gdl::game {
AutoSaveResult autoSaveParty(SaveSlots& slots, std::span<PartyMember> party) {
    AutoSaveResult result;
    if (!slots.opened()) {
        result.unavailable = party.size();
        return result;
    }
    slots.refresh();
    for (auto& member : party) {
        auto destination = member.slot;
        if (destination && (*destination >= slots.count() ||
                            std::ranges::count(party, destination, &PartyMember::slot) != 1)) {
            ++result.unavailable;
            continue;
        }
        if (!destination) {
            for (usize index = 0; index < slots.count(); ++index) {
                const bool claimed = std::ranges::any_of(
                    party, [index](const PartyMember& other) { return other.slot == index; });
                if (!claimed && !slots.slot(index).occupied) {
                    destination = index;
                    break;
                }
            }
        }
        const usize selectedSlot = destination.value_or(slots.count());
        if (selectedSlot >= slots.count()) {
            ++result.unavailable;
        } else if (slots.write(selectedSlot, member.save)) {
            member.slot = destination;
            ++result.written;
        } else {
            ++result.failed;
        }
    }
    return result;
}
} // namespace gdl::game
