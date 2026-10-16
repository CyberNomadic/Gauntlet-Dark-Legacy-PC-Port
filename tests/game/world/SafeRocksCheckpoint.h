#pragma once

#include <optional>

#include "game/world/SafeRocks.h"

namespace gdl::game {

/** Complete static arena-cover state with preloaded local models and texture banks.
 * No eruption effects, damage dispatch, model loading or texture uploads occur on restore. */
class SafeRocksCheckpoint {
public:
    struct Rock {
        s32 instance = -1;
        ItemInfo definition;
        ItemInstance source;
        s32 health = 0;
        s32 baseHealth = 0;
        s32 armor = 0;
        s32 tier = 0;
        s32 minPlayers = 0;
        bool shown = true;
        bool dormant = false;
        f32 activationDelay = 0;
        Vec3 position{0};
        Obstacle obstacle;
        Mat4 placement{1};
        std::array<bool, 4> models{};
        bool operator==(const Rock&) const;
    };
    struct State {
        static constexpr usize kMaxRocks = 4096;
        static constexpr usize kMaxSlots = 65535;
        static constexpr usize kMaxAnimations = 65535;
        static constexpr usize kMaxChoices = 65535;
        static constexpr usize kMaxCollisionTriangles = 65535;
        std::vector<Rock> rocks;
        std::vector<u32> textureSlots;
        std::vector<TextureAnimationInfo> animations;
        usize resolvedAnimations = 0;
        TextureAnimator::Checkpoint textures;
        bool layout = false;
        bool items = false;
        bool animationItems = false;
        bool valid() const;
        bool operator==(const State&) const;
    };
    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        friend class SafeRocksCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };

    explicit SafeRocksCheckpoint(SafeRocks& owner);
    ~SafeRocksCheckpoint();
    SafeRocksCheckpoint(const SafeRocksCheckpoint&) = delete;
    SafeRocksCheckpoint& operator=(const SafeRocksCheckpoint&) = delete;
    SafeRocksCheckpoint(SafeRocksCheckpoint&&) = delete;
    SafeRocksCheckpoint& operator=(SafeRocksCheckpoint&&) = delete;

    std::optional<State> checkpoint() const;
    std::optional<Prepared> prepareRestore(const State& state) const;
    bool canCommit(const Prepared& prepared) const;
    /** Keeps the addresses exported by bodyResources() stable. Clear, model or
     * texture-bank rebinding and sibling restores invalidate prepared plans.
     * Immutable borrowed layouts, archives and lenders must outlive both owners. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state);

private:
    SafeRocks& m_owner;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};
} // namespace gdl::game
