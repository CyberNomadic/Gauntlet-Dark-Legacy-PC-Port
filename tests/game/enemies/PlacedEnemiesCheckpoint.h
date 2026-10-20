#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "game/enemies/PlacedEnemies.h"

namespace gdl::game {

/** Legacy test-only, pointer-free diagnostic records. Shipping rewind uses the
 * owner's actual LocalSnapshot; none of these records are runtime state. */
class PlacedEnemiesCheckpoint {
public:
    struct State {
        static constexpr usize kMaxPlacements = 65535;
        std::vector<PlacedEnemies::Placement> roster;
        std::vector<usize> pending;
        std::optional<ViewVolume> view;
        Vec3 attention{0};
        bool valid() const;
        bool operator==(const State& /*other*/) const;
        std::optional<std::vector<PlacedEnemies::Placement>> activations() const;
        std::optional<std::vector<PlacedEnemies::Placement>>
        activations(std::optional<ViewVolume> nextView, const Vec3& nextAttention) const;
    };

    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        friend class PlacedEnemiesCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };

    static bool validPlacement(const PlacedEnemies::Placement& value);
    static std::optional<State> capture(const PlacedEnemies& owner);
    static std::optional<Prepared> prepareRestore(const PlacedEnemies& owner, const State& state);
    /** Content-based preparation guard, not a count of mutations. An identical
     * observe/activate or a native rewind back to the baseline is still valid. */
    static bool canCommit(const PlacedEnemies& owner, const Prepared& prepared);
    static bool commitRestore(PlacedEnemies& owner, Prepared&& prepared);
    static bool restore(PlacedEnemies& owner, const State& state);
};

} // namespace gdl::game
