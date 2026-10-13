#include "game/netplay/ShotIdentity.h"

#include <tuple>

#include "game/netplay/InputCommand.h"
#include "game/players/TurboMove.h"

namespace gdl::game {
bool ShotIdentity::valid() const {
    const u32 slot = emission >> 16U;
    return epoch != 0 && generation != 0 && grant != 0 && seat < InputCommand::kSeats &&
           ordinal < kMaxShots &&
           (emission == 0 ||
            (!strong && ordinal == 0 && slot >= 1 && slot <= TurboMove::kMaxCheckpointRows));
}
std::strong_ordering ShotIdentity::operator<=>(const ShotIdentity& other) const {
    return std::tuple{epoch,   tick,    generation, grant, seat, static_cast<u8>(strong),
                      ordinal, emission} <=>
           std::tuple{other.epoch,   other.tick,    other.generation,
                      other.grant,   other.seat,    static_cast<u8>(other.strong),
                      other.ordinal, other.emission};
}
} // namespace gdl::game
