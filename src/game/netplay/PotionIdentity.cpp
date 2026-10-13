#include "game/netplay/PotionIdentity.h"

#include "game/netplay/InputCommand.h"

namespace gdl::game {
bool PotionIdentity::valid() const {
    return epoch != 0 && generation != 0 && grant != 0 && seat < InputCommand::kSeats &&
           (kind == Kind::Use || kind == Kind::Throw);
}
} // namespace gdl::game
