#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <optional>

#include "engine/core/Types.h"

#include "game/netplay/MatchSession.h"

namespace gdl::game {
class OnlineCombatReplay;
class StageParticlePresentation;

/** Detached, main-thread Stage sampling. Native simulation may advance or rewind
 * after publication; no sampling operation reads a CombatPrediction owner.
 * A resource token is an invalidation fence, NOT an asset lease. Before releasing
 * stage artwork, stop/join its producer, release every renderer/publication, then
 * destroy the loaded owners on the application thread. Never race teardown with
 * sampling. Sidecars have main-thread draw caches despite their const interface. */
class StageReplayPresentation {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    enum class Refusal { None, Inactive, Authority, Snapshot, Roster, ParticleInterval, Sampling };
    static constexpr u64 kMaxTicks = 120;

    class Authority {
    public:
        u64 epoch() const { return m_context.epoch; }
        u64 tick() const { return m_tick; }

    private:
        friend class StageReplayPresentation;
        struct Seat {
            u32 continuity = 0;
            ReplicaPlayerLife life = ReplicaPlayerLife::Standing;
            bool operator==(const Seat&) const = default;
        };
        MatchContext m_context;
        u8 m_local = 0;
        u64 m_tick = 0;
        u32 m_cameraContinuity = 0;
        std::array<std::optional<Seat>, InputCommand::kSeats> m_seats;
        std::weak_ptr<const void> m_resources;
    };

    /** Validate once per completed authority, not once per render. The key keeps
     * no gameplay supplement or mutable native owner. */
    static std::optional<Authority> authority(const MatchContext& context, u8 localPeer,
                                              const CombatSnapshot& snapshot,
                                              std::weak_ptr<const void> resources);

    class Publication {
    public:
        const Authority& authority() const { return m_authority; }
        u64 tick() const { return m_current ? m_current->motion.tick : 0; }
        u64 revision() const { return m_revision; }

    private:
        friend class StageReplayPresentation;
        Authority m_authority;
        u64 m_revision = 0;
        std::unique_ptr<const CombatSnapshot> m_previous, m_current;
        std::shared_ptr<const StageParticlePresentation> m_previousParticles, m_currentParticles;
    };

    /** Copies exactly two drawing endpoints. Sidecars already own their native
     * samples and point-light lists and are shared without copying particle fields. Publication
     * is bounded and immutable; only this factory can populate its contents. */
    static std::shared_ptr<const Publication>
    publish(const Authority& identity, u64 revision, const CombatSnapshot& previous,
            const CombatSnapshot& current,
            std::shared_ptr<const StageParticlePresentation> previousParticles,
            std::shared_ptr<const StageParticlePresentation> currentParticles);

    /** Compatibility route for the synchronous coordinator: the same detached
     * sampler over stable borrowed endpoints, without a copy on every draw. */
    static bool sample(CombatSnapshot& shown, EnemyPresentation& presentation,
                       const Authority& identity, const CombatSnapshot& previous,
                       const CombatSnapshot& current,
                       const std::shared_ptr<const StageParticlePresentation>& previousParticles,
                       const std::shared_ptr<const StageParticlePresentation>& currentParticles,
                       f32 blend, Refusal* refusal = nullptr);

    /** Main-thread adoption checks exact authority and resource generation.
     * Readopting the same revision does not restart its interpolation clock.
     * Failed adoption withdraws any older publication. */
    bool adopt(std::shared_ptr<const Publication> publication, const Authority& current,
               Time now = Clock::now());
    /** Uses a monotonic per-adoption fraction, then holds its certified endpoint.
     * The application's cycling frameBlend must not rewind a retained interval. */
    bool present(CombatSnapshot& shown, EnemyPresentation& presentation, const Authority& current,
                 Time now = Clock::now(), Refusal* refusal = nullptr);
    void clear();
    bool active() const;

private:
    friend class OnlineCombatReplay;
    // Coordinator authority was already fully validated on receipt. Keep its
    // synchronous render path free of repeated full native-seed validation.
    static std::optional<Authority> validatedAuthority(const MatchContext& context, u8 localPeer,
                                                       const CombatSnapshot& snapshot,
                                                       std::weak_ptr<const void> resources);
    static bool sameAuthority(const Authority& first, const Authority& second);
    static bool interval(const Authority& identity, const CombatSnapshot& previous,
                         const CombatSnapshot& current);
    std::shared_ptr<const Publication> m_publication;
    Time m_adopted{};
    f32 m_blend = 0;
};
} // namespace gdl::game
