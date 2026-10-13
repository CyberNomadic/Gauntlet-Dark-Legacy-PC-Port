#include "game/netplay/EffectIdentity.h"

#include "game/netplay/InputCommand.h"

namespace gdl::game {
bool EffectIdentity::valid() const {
    return epoch != 0 && generation != 0 && grant != 0 && seat < InputCommand::kSeats &&
           occurrence < kMaxOccurrences && effectOrdinal < kMaxEffects;
}
} // namespace gdl::game
