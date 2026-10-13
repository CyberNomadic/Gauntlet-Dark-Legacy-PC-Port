#pragma once

#include <array>
#include <map>
#include <optional>
#include <span>

#include "engine/core/Types.h"

#include "game/netplay/InputCommand.h"
#include "game/netplay/ShotIdentity.h"

namespace gdl::game {
/** Reconciles visual-only weapon predictions with complete host projectile
 * rosters. It owns no damage, inventory, physics or audiovisual event callbacks.
 * The renderer replaces buffered local-origin shots with pending visuals or
 * the newest host visuals; retired keys prevent old buffered resurrection. */
class PredictedShotLedger {
public:
    PredictedShotLedger() = default;
    ~PredictedShotLedger() = default;
    PredictedShotLedger(const PredictedShotLedger&) = default;
    PredictedShotLedger& operator=(const PredictedShotLedger&) = default;
    // std::map's move may allocate its sentinel on MSVC.
    PredictedShotLedger(PredictedShotLedger&&) noexcept(false) = default;
    PredictedShotLedger& operator=(PredictedShotLedger&&) noexcept(false) = default;

    static constexpr u64 kHorizon = 120;
    // The ordinary-shot budget covers five members per tick for all local seats,
    // plus equally sized confirmed/retired history while rendering catches up.
    // Authored turbo rows can burst above five emissions in one tick: this is a
    // bounded budget, not a bound on native gameplay. Saturation disables local
    // speculation rather than discarding unresolved identities to make room.
    static constexpr usize kCapacity =
        2 * kHorizon * InputCommand::kSeats * ShotIdentity::kMaxShots;
    static constexpr u64 kPrivateInstance = u64{1} << 63;
    using Grants = std::array<u32, InputCommand::kSeats>;
    enum class Status : u8 { Pending, Confirmed, Retired };
    struct Entry {
        u64 instance = 0; ///< private stable ID for speculative render components
        Status status = Status::Pending;
        std::optional<u64> retiredAt;
    };

    /** A zero grant denotes a seat not owned locally. Epoch changes clear all
     * history; changed grants clear only those seats. */
    bool begin(u64 epoch, const Grants& grants);
    void clear();
    /** Newest complete post-step authoritative live roster. Repeated component
     * origins are allowed. Old/duplicate checkpoints cannot remove or resurrect
     * shots. An absent speculative release is retired once its exact tick passes,
     * including shots that were born and died between transmitted snapshots. */
    bool observe(u64 epoch, u64 postStepTick, std::span<const ShotIdentity> liveOrigins);
    /** Replace the entire seat's replay release list after restoring the current
     * authoritative post-step baseline. No already-resolved tick can be replayed.
     * Call even for an empty replay; use withdraw when prediction is unavailable. */
    bool replace(u8 seat, u64 postStepTick, u64 endTick,
                 std::span<const ShotIdentity> replayReleases);
    /** Revoke a seat's pending speculation without hiding its current host shots. */
    void withdraw(u8 seat);
    /** Keep terminal keys until the delayed render buffer has passed their fence.
     * The displayed cursor must never be newer than observed authority. */
    bool advanceDisplayed(u64 epoch, u64 tick);
    bool suppress(const ShotIdentity& origin) const;
    const std::map<ShotIdentity, Entry>& entries() const { return m_entries; }
    std::optional<u64> resolved() const { return m_resolved; }
    bool failed() const { return m_failed; }
    u64 epoch() const { return m_epoch; }

private:
    bool owned(const ShotIdentity& origin) const;
    bool admit(usize additions);
    void retire(const ShotIdentity& origin, Entry& entry);
    void prune();
    std::map<ShotIdentity, Entry> m_entries;
    Grants m_grants{};
    u64 m_epoch = 0;
    u64 m_nextInstance = kPrivateInstance;
    std::optional<u64> m_resolved;
    std::optional<u64> m_displayed;
    bool m_failed = false;
};
} // namespace gdl::game
