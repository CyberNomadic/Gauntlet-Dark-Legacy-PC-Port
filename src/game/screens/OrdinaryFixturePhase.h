#pragma once

#include "game/screens/LevelFixtures.h"

namespace gdl::game {
/** Runs the native fixture phase up to its first unsupported external event.
 * This is deliberately not an independent transaction: the caller must hold
 * one joint player/world/fixture/effect rewind before calling. A refusal may
 * follow contact resolution, key consumption and leaf clock/RNG changes, but
 * never publishes sound, help, a pickup, damage or a resource allocation.
 * Native keyed gates may proceed with buffered named sound; a refused gate
 * requires proof that its help request is already suppressed. All other
 * external fixture boundaries retain their explicit refusal. */
class OrdinaryFixturePhase {
public:
    static LevelFixtures::Phase advance(LevelFixtures& fixtures, s32 ticks, f32 seconds,
                                        std::span<PlayerRuntime> party);
};
} // namespace gdl::game
