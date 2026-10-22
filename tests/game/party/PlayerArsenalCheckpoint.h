#pragma once

#include "game/party/PlayerArsenal.h"

namespace gdl::game {
/** Resumable mutable arsenal state. Artwork and callback observers stay bound to
 * the owner; shared world effects belong to the enclosing transaction. */
class PlayerArsenalCheckpoint {
public:
    struct State {
        struct Cast {
            s32 owner = -1;
            u32 effect = 0;
            bool operator==(const Cast&) const = default;
        };
        static constexpr usize kMaxCasts = 4096;
        bool bound = false;
        s32 nextPotionKind = 1;
        std::vector<Cast> casts;
        PlayerMissiles::Checkpoint missiles;
        bool valid() const;
        bool operator==(const State&) const = default;

    private:
        friend class PlayerArsenalCheckpoint;
        bool validAggregate() const;
    };
    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        friend class PlayerArsenalCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };
    static std::optional<State>
    capture(const PlayerArsenal& owner, std::span<const PlayerMissiles::Resource> resources = {},
            std::span<const EffectTrees::PoolResource> missileEffects = {});
    static std::optional<Prepared>
    prepare(PlayerArsenal& owner, const State& state, std::span<const s32> players,
            const EffectTrees::PoolCheckpoint& sharedEffects,
            std::span<const PlayerMissiles::Resource> resources = {},
            std::span<const EffectTrees::PoolResource> missileEffects = {});
    static bool canCommit(const PlayerArsenal& owner, const Prepared& prepared) noexcept;
    static bool commit(PlayerArsenal& owner, Prepared&& prepared) noexcept;
    static bool restore(PlayerArsenal& owner, const State& state, std::span<const s32> players,
                        const EffectTrees::PoolCheckpoint& sharedEffects,
                        std::span<const PlayerMissiles::Resource> resources = {},
                        std::span<const EffectTrees::PoolResource> missileEffects = {});
};
} // namespace gdl::game
