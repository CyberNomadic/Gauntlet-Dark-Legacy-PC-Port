#pragma once

#include "game/netplay/StageStatePacket.h"
#include "game/screens/OrdinaryCombatTick.h"

namespace gdl::game {
/** Probabilistic SHA-256 agreement between already validated native boundaries.
 * No compression, object padding, pointer identity or resource enumeration is
 * hashed. A digest never replaces incoming validation or trusted preparation.
 * Only draw-overwritten ItemFigure/PlacedItems pose caches and Stage point-light
 * lists are normalized. Every simulation pose/clock/previous sample remains. */
class CombatAgreement {
public:
    using Digest = std::array<u8, 32>;
    static std::optional<Digest> state(const OrdinaryCombatCheckpoint::State& state,
                                       const StageStatePacket::Catalog& catalog);
    /** Native Stage camera overrides supplied camera/attention before simulation.
     * Without that participant, these observations remain part of agreement. */
    static std::optional<Digest> context(const OrdinaryCombatTick::Frame& frame, bool nativeCamera);
};
} // namespace gdl::game
