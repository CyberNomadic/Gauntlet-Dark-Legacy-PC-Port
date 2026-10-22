#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "game/level/Chests.h"
#include "game/level/ItemFigureCheckpoint.h"

namespace gdl::game {

/** Complete ordered container state. Item records and figure layouts are resolved
 * against this owner's already-loaded roster; restoring never opens a chest,
 * spends keys, grants gold, loads art, or dispatches a ChestEvent. */
class ChestsCheckpoint {
public:
    using Figure = ItemFigureCheckpoint::State;
    struct Floor {
        s32 object = -1;
        Mat4 local{1};
        bool operator==(const Floor&) const = default;
    };
    struct Chest {
        s32 instance = -1;
        s32 info = -1;
        s32 subtype = 0;
        s32 contents = -1;
        s32 count = 0;
        bool locked = true;
        s32 state = Chests::kShut;
        s32 opener = -1;
        f32 refusalLeft = 0;
        s32 minPlayers = 0;
        bool shown = true;
        s32 held = -1;
        bool gone = false;
        bool revealed = false;
        s32 previewContents = -1;
        f32 wobble = 0;
        Figure preview;
        Figure figure;
        Obstacle box;
        std::optional<Floor> floor;
        std::optional<f32> collectedSeconds;
        bool operator==(const Chest& /*other*/) const;
    };
    struct State {
        static constexpr usize kMaxChests = 4096;
        static constexpr usize kMaxInfos = 65535;
        static constexpr usize kMaxChoices = 65535;
        static constexpr usize kMaxCollisionTriangles = 65535;
        static constexpr usize kMaxName = 64;
        /** Immutable definition identity, including recursive choice-list order.
         * Equal chest counts alone cannot authenticate a random-contents roster. */
        std::vector<ItemInfo> infos;
        /** The complete immutable authored instance for each ordered chest. */
        std::vector<ItemInstance> placements;
        std::vector<Chest> chests;
        u32 seed = 0;
        bool collision = false;
        bool layout = false;
        bool valid() const;
        bool operator==(const State& /*other*/) const;
    };
    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        friend class ChestsCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };

    explicit ChestsCheckpoint(Chests& owner);
    ChestsCheckpoint(const ChestsCheckpoint&) = delete;
    ChestsCheckpoint& operator=(const ChestsCheckpoint&) = delete;
    ChestsCheckpoint(ChestsCheckpoint&&) = delete;
    ChestsCheckpoint& operator=(ChestsCheckpoint&&) = delete;
    ~ChestsCheckpoint();

    std::optional<State> checkpoint() const;
    std::optional<Prepared> prepareRestore(const State& state) const;
    bool canCommit(const Prepared& prepared) const;
    /** After preflight, this is allocation/callback-free. Native container steps
     * may run between preparation and commit (for undo); clear/rebind or another
     * successful restore invalidates the plan. Borrowed archives and collision
     * must remain alive and unchanged in binding until the plan is released. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state);

private:
    Chests& m_owner;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};

} // namespace gdl::game
