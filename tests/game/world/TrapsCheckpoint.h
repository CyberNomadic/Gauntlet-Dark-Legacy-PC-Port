#pragma once

#include "game/world/ItemFigureCheckpoint.h"
#include "game/world/Traps.h"

namespace gdl::game {
/** Complete trap phase, including victim cooldowns, wakes, random rests and
 * disarmed replacement figures. Floor transforms and collision are external
 * owners and must be restored by the enclosing world transaction. */
class TrapsCheckpoint {
public:
    struct Support {
        s32 object = -1;
        Mat4 local{1};
        Vec3 boxOffset{0};
        f32 boxYaw = 0;
        bool operator==(const Support&) const = default;
    };
    struct Trap {
        s32 instance = -1;
        ItemInfo definition;
        ItemInstance placement;
        f32 damage = 0;
        s32 offTime = 0;
        s32 subtype = 0;
        u32 properties = 0;
        s32 action = 0;
        s32 ticksLeft = 0;
        s32 minPlayers = 0;
        bool shown = true;
        bool disarmed = false;
        bool gone = false;
        bool rectangularContact = false;
        ItemFigureCheckpoint::State figure;
        Obstacle box;
        Support support;
        bool operator==(const Trap& /*other*/) const;
    };
    struct State {
        static constexpr usize kMaxTraps = 4096;
        std::vector<Trap> traps;
        std::vector<usize> wakes;
        std::vector<f32> gaps;
        u32 random = 1;
        f32 timeScale = 1;
        bool collision = false;
        bool layout = false;
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
        friend class TrapsCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };
    explicit TrapsCheckpoint(Traps& owner);
    ~TrapsCheckpoint();
    TrapsCheckpoint(const TrapsCheckpoint&) = delete;
    TrapsCheckpoint& operator=(const TrapsCheckpoint&) = delete;
    /** Load-time only: prebind both authored and disarmed variants. Missing
     * disarmed art follows the native gone-state path, not an invented model. */
    bool bindResources(RenderDevice& device);
    std::optional<State> checkpoint() const;
    std::optional<Prepared> prepareRestore(const State& state) const;
    bool canCommit(const Prepared& prepared) const;
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state);

private:
    struct Resources;
    bool bound() const;
    Traps& m_owner;
    std::unique_ptr<Resources> m_resources;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};
} // namespace gdl::game
