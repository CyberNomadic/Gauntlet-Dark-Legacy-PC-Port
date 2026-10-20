#pragma once

#include <memory>
#include <optional>

#include "game/world/FallingScenery.h"
#include "game/world/FixtureCheckpointDetail.h"

namespace gdl::game {
/** Complete fixed-roster falling motion, including the private previous pose used
 * by rendering. Starting cues remain explicit native outputs, never restore effects. */
class FallingSceneryCheckpoint {
public:
    struct Piece {
        fixture_checkpoint::Placement source;
        Vec3 position{0}, rotation{0}, velocity{0};
        Vec3 previousPosition{0}, previousRotation{0};
        bool visible = true;
        f32 gravityStep = 0, spin = 0;
        s32 subtype = 0, minPlayers = 0;
        f32 radius = 0, height = 0;
        Vec3 centre{0};
        bool shown = true, started = false;
        bool operator==(const Piece&) const = default;
    };
    struct State {
        static constexpr usize kMaxPieces = 4096;
        static constexpr usize kMaxDetails = 65535;
        std::vector<Piece> pieces;
        std::string breakSound, leafSound;
        f32 remainder = 0, updateSeconds = 0, bottom = 0;
        bool bound = false;
        bool valid() const;
        bool operator==(const State&) const = default;
    };
    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        friend class FallingSceneryCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };
    explicit FallingSceneryCheckpoint(FallingScenery& owner) : m_owner(owner) {}
    FallingSceneryCheckpoint(const FallingSceneryCheckpoint&) = delete;
    FallingSceneryCheckpoint& operator=(const FallingSceneryCheckpoint&) = delete;
    ~FallingSceneryCheckpoint() = default;
    std::optional<State> checkpoint() const;
    std::optional<Prepared> prepareRestore(const State& state) const;
    bool canCommit(const Prepared& prepared) const;
    /** No allocations, cues or model rebinding; preserves fixed native piece/model addresses. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state);

private:
    FallingScenery& m_owner;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};
} // namespace gdl::game
