#pragma once

#include "game/screens/PlayerAttacks.h"

namespace gdl::game {
/** Value boundary for the attack owner's transient gameplay. The shared effect
 * pool is prepared separately exactly once by the surrounding transaction; this
 * owner validates references against that same pool checkpoint, never restores
 * it a second time or calls clear()/gameplay callbacks during commit. */
class PlayerAttacksCheckpoint {
public:
    struct Actor {
        s32 player = -1;
        s32 character = -1;
        bool operator==(const Actor&) const = default;
    };
    enum class Item : u8 {
        Horns,
        Mask,
        LeftGauntlet,
        RightGauntlet,
        SuperShot,
        Hammer,
        FireBreath,
        AcidBreath,
        LightningBreath
    };
    struct State {
        static constexpr usize kMaxAreas = 4096;
        static constexpr usize kMaxContacts = 65536;
        struct ItemArea {
            Item item = Item::FireBreath;
            usize actor = 0;
            u32 effect = 0;
            f32 elapsed = 0, lifetime = 1;
            bool pojoTurbo = false;
            std::vector<s32> hit;
            bool operator==(const ItemArea&) const = default;
        };
        struct Potion {
            PlayerMissiles::Checkpoint::Impact impact;
            f32 elapsed = 0, duration = 1;
            std::vector<s32> hit, blessed;
            bool struck = false;
            bool operator==(const Potion&) const = default;
        };
        struct StrikeEffect {
            u32 strike = 0, effect = 0;
            bool operator==(const StrikeEffect&) const = default;
        };
        struct StrikeSource {
            struct Contact {
                s32 target = -1;
                f32 remaining = 0;
                bool operator==(const Contact&) const = default;
            };
            u32 strike = 0;
            usize actor = 0;
            s32 row = -1;
            std::vector<Contact> contacts;
            MultiplayerMode multiplayer = MultiplayerMode::Normal;
            u32 damageType = 0;
            bool operator==(const StrikeSource&) const = default;
        };
        struct Attachment {
            usize actor = 0;
            u32 effect = 0, strike = 0;
            Mat4 local{1};
            bool bodyParent = false;
            bool operator==(const Attachment&) const = default;
        };
        struct Particle {
            usize actor = 0;
            u32 effect = 0, parent = 0;
            std::string node;
            Vec3 offset{0};
            bool operator==(const Particle&) const = default;
        };
        struct Shield {
            struct Mark {
                s32 kind = 0, id = 0;
                f32 wait = 0;
                bool operator==(const Mark&) const = default;
            };
            usize actor = 0;
            u32 flags = 0, effect = 0;
            f32 radius = 0, damage = 0, secondsLeft = 0;
            std::vector<Mark> marks;
            std::vector<s32> blessed;
            bool operator==(const Shield&) const = default;
        };
        bool bound = false;
        std::vector<Actor> actors;
        MoveStrikes::Checkpoint strikes;
        std::vector<ItemArea> items;
        std::vector<Potion> potions;
        std::vector<StrikeEffect> strikeEffects;
        std::vector<StrikeSource> strikeSources;
        std::vector<Attachment> attachments;
        std::vector<Particle> particles;
        std::vector<Shield> shields;
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
        friend class PlayerAttacksCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };
    static std::optional<State> capture(const PlayerAttacks& owner, std::span<const Actor> actors,
                                        std::span<const PlayerMissiles::Resource> resources = {});
    /** Actor bindings are the same ordered party snapshot restored by the
     * transaction, not invented from actor indices or current network seat order. */
    static std::optional<Prepared>
    prepare(PlayerAttacks& owner, const State& state, std::span<const Actor> actors,
            const EffectTrees::PoolCheckpoint& effects,
            std::span<const PlayerMissiles::Resource> resources = {});
    static bool canCommit(const PlayerAttacks& owner, const Prepared& prepared) noexcept;
    static bool commit(PlayerAttacks& owner, Prepared&& prepared) noexcept;
    static bool restore(PlayerAttacks& owner, const State& state, std::span<const Actor> actors,
                        const EffectTrees::PoolCheckpoint& effects,
                        std::span<const PlayerMissiles::Resource> resources = {});
};
} // namespace gdl::game
