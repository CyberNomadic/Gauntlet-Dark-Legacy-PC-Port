#pragma once

#include "game/screens/CombatRollbackCheckpoint.h"

namespace gdl::game {

/** Native free-A strong-throw emissions inside an enclosing complete combat
 * transaction. This is not the B/C presentation-only pool: strike carriers,
 * attachments and particles are allocated in the actual checkpointed owners.
 * No audio, lesson, camera or resource binding is dispatched. The guarded strike
 * phase mutates the native swarm and its owned queues under the caller's undo. */
class OrdinaryStrongPhase {
public:
    struct Cue {
        s32 owner = -1;
        u64 generation = 0;
        u16 occurrence = 0;
        u16 ordinal = 0;
        std::string name;
        std::optional<u32> classCall;
        bool operator==(const Cue&) const = default;
    };
    struct Result {
        bool advanced = false;
        std::vector<Cue> cues;
    };
    /** Persistent carrier/attachment state must belong to the native free-A
     * rows. This does not certify future contacts; stepStrikes checks those. */
    static bool supported(const PlayerAttacks& attacks, std::span<const PlayerRuntime> party);
    /** Includes unparented/retiring tails after their strike ledgers expire. */
    static bool supportsEffect(const PlayerAttacks& attacks,
                               const EffectTrees::PoolResource& resource,
                               std::span<const PlayerRuntime> party);

    /** Only a native StrongThrow timeline occurrence is admitted. The complete
     * effect chain is authenticated against trusted load-time prototypes before
     * the first start. A failure after allocation must be undone by the caller's
     * joint rewind, and its partial outbox must never be published. */
    static Result emit(PlayerAttacks& attacks, usize index, const TurboMove::Strike& strike,
                       std::span<PlayerRuntime> party,
                       const CombatRollbackCheckpoint::Resources& resources);
    /** The caller applies the native crowd/party/known-message policy. False
     * means a real uncaptured lesson could be posted and refuses the phase;
     * accepting a callback must never mean silently dropping that lesson. */
    static Result advance(PlayerAttacks& attacks, usize index, s32 ticks, f32 seconds,
                          std::span<PlayerRuntime> party,
                          const CombatRollbackCheckpoint::Resources& resources,
                          const std::function<bool(s32, usize)>& help);
    /** Shared native attachment/area-flight/contact phase, restricted to the
     * ordinary swarm. Late refusal is private and requires joint rewind. Native
     * hit/kill/feedback queues remain in their original next-tick owners. */
    static bool stepStrikes(PlayerAttacks& attacks, f32 seconds, std::span<PlayerRuntime> party,
                            const PlayerAttacks::Targets& targets);
};

} // namespace gdl::game
