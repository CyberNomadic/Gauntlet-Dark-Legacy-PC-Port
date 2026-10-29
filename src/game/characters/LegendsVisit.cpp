#include "game/characters/LegendsVisit.h"

namespace gdl::game {

bool commitLegendsVisit(CharacterSave& live, const CharacterSave& before,
                        const CharacterSave& after) {
    if (live != before || after.name != before.name || after.character != before.character ||
        after.color != before.color || after.character < 0 || after.character >= kClassCount) {
        return false;
    }
    for (const auto& progress : after.classes) {
        for (const auto& campaign : progress.legends) {
            if (!campaign.valid()) {
                return false;
            }
        }
    }
    live = after;
    return true;
}

} // namespace gdl::game
