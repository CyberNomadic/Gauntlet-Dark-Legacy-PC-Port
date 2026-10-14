#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "game/enemies/Enemies.h"

namespace gdl::game {

/** The ordered, resolved placement records waiting for the native watch phase.
 * The complete initial roster stays trusted and immutable: a correction may
 * restore consumed records, never introduce a packet-authored enemy or item. */
class PlacedEnemies {
public:
    static constexpr f32 kWatchReach = 50;

    struct Placement {
        EnemySpawn spawn;
        s32 kind = -1;
        f32 facing = 0;
        f32 viewRadius = 0;
        f32 sight = 0;
        std::optional<usize> carried;
        bool valid() const;
        bool operator==(const Placement&) const;
    };

    struct Checkpoint {
        static constexpr usize kMaxPlacements = 65535;
        std::vector<Placement> roster;
        /** Native erasure preserves original order, including identical records. */
        std::vector<usize> pending;
        std::optional<ViewVolume> view;
        Vec3 attention{0};
        bool valid() const;
        bool operator==(const Checkpoint&) const;
        /** Pure native watch-phase selection; malformed state/view refuses. The
         * overload preflights a prospective watch without mutating this value. */
        std::optional<std::vector<Placement>> activations() const;
        std::optional<std::vector<Placement>> activations(std::optional<ViewVolume> nextView,
                                                          const Vec3& nextAttention) const;
    };

    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
        friend class PlacedEnemies;
    };
    class Rewind {
    public:
        Rewind(Rewind&&) noexcept;
        Rewind& operator=(Rewind&&) noexcept;
        ~Rewind();
        Rewind(const Rewind&) = delete;
        Rewind& operator=(const Rewind&) = delete;

    private:
        friend class PlacedEnemies;
        explicit Rewind(Prepared&& state);
        Prepared m_state;
    };

    PlacedEnemies() = default;
    PlacedEnemies(const PlacedEnemies&) = delete;
    PlacedEnemies& operator=(const PlacedEnemies&) = delete;
    PlacedEnemies(PlacedEnemies&&) = delete;
    PlacedEnemies& operator=(PlacedEnemies&&) = delete;

    /** Load-time records have already passed the level's native resource and
     * party filters. Checkpoint bounds do not change ordinary loading behavior. */
    void bind(std::vector<Placement> roster);
    void clear();
    void observe(std::optional<ViewVolume> view, const Vec3& attention);
    bool inView(const Vec3& position, f32 radius) const;
    usize count() const { return m_pending.size(); }
    std::span<const Placement> roster() const { return m_roster; }
    std::span<const usize> pending() const { return m_pending; }

    /** Read-only preflight in native order, with the same priority override as
     * activate. A private owner can reject an unsupported birth before mutation. */
    std::vector<Placement> activations() const;
    /** Calls the native spawn dispatcher synchronously, then consumes each seen
     * record even when the spawn failed. No-view loading only uses free slots;
     * watched placements retain their authored replacement priority. */
    void activate(const std::function<void(const Placement&)>& stand);

    std::optional<Checkpoint> checkpoint() const;
    std::optional<Prepared> prepareRestore(const Checkpoint& state) const;
    bool canCommit(const Prepared& prepared) const;
    /** Allocation-free after preparation, without spawning or dispatching callbacks. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const Checkpoint& state);
    /** Undo the current placement phase after observe/activate. The enclosing
     * transaction must also undo any pools changed by the spawn callback. A
     * bind, clear, or successful restore/rewind invalidates every sibling. */
    std::optional<Rewind> prepareRewind() const;
    /** Validated historical target; preparation never changes the live watch. */
    std::optional<Rewind> prepareRewind(const Checkpoint& target) const;
    bool canRewind(const Rewind& rewind) const;
    bool rewind(Rewind&& rewind);

private:
    std::optional<Placement> activation(usize index) const;
    void commitReady(Prepared&& prepared);
    std::vector<Placement> m_roster;
    std::vector<usize> m_pending;
    std::optional<ViewVolume> m_view;
    Vec3 m_attention{0};
    bool m_bound = false;
    u64 m_generation = 0;
    u64 m_rewindGeneration = 0;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};

} // namespace gdl::game
