#include "game/rollback/PotionIdentity.h"

#include "game/rollback/InputCommand.h"

namespace gdl::game {
bool PotionIdentity::valid() const {
    return epoch != 0 && generation != 0 && grant != 0 && seat < InputCommand::kSeats &&
           (kind == Kind::Use || kind == Kind::Throw);
}
} // namespace gdl::game
