#pragma once

#include <array>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "game/players/Inventory.h"
#include "game/players/PlayerAnimator.h"
#include "game/world/PotionEmission.h"

namespace gdl::game {
/** A private fixed-60-Hz action replay, not an inventory or damage owner. The
 * movement adapter supplies native priority-selected deeds and post-motion
 * positions. This class never samples devices, chooses targets, dispatches audio,
 * launches a live missile or mutates a CharacterSave. */
class PotionSimulation {
public:
    static constexpr u32 kMaxTicks = 120;
    static constexpr f32 kStep = 1.0f / 60;
    struct Binding {
        u64 epoch = 0;
        u32 grant = 0;
        u8 seat = 0;
        bool valid() const;
        bool operator==(const Binding&) const = default;
    };
    struct Context {
        /** Exact native potionPowerOf / colourBonus indexed by the stored kind,
         * not the resolved colour. The adapter certifies these and the attack
         * modifiers remain valid for the entire remaining replay prefix. */
        std::array<f32, 5> powers{8, 8, 8, 8, 8};
        std::array<f32, 5> bonuses{1, 1, 1, 1, 1};
        u32 damageFlags = 0;
        std::optional<u8> nextKind; ///< exact shared resolver checkpoint, never inferred
        bool rapid = false;
        bool speed = false;
        bool valid() const;
        bool operator==(const Context&) const = default;
    };
    struct Checkpoint {
        u64 tick = 0; ///< completed authoritative/replayed simulation tick
        PlayerAnimator::Checkpoint animation;
        std::optional<PlayerAnimator::TransitionPose> transitionPose;
        std::vector<s32> potions; ///< at most nine; native last-in first-out order
        Context context;
        /** A reaction already known at the checkpoint, not a predicted attack
         * against this actor. A new hurt/death requires a corrected checkpoint. */
        PlayerDeed reaction = PlayerDeed::None;
        u32 remaining = kMaxTicks;
        bool valid() const;
        bool operator==(const Checkpoint&) const = default;
    };
    struct Frame {
        u64 tick = 0;
        PlayerMotion motion = PlayerMotion::Stand;
        StrafeWay strafe = StrafeWay::None;
        /** Already selected by native PartyMotion priority and carrying checks.
         * Unsupported higher-priority turbo/shield/combo/item deeds reject the
         * frame; they must never be silently replaced by a potion request. */
        PlayerDeed deed = PlayerDeed::None;
        Vec3 position{0};
        Vec3 facing{0, 0, 1};
    };
    enum class Kind : u8 { Use, Throw };
    struct Identity {
        Binding owner;
        u64 tick = 0;
        u64 generation = 0;
        Kind kind = Kind::Use;
        bool valid() const;
        bool operator==(const Identity&) const = default;
    };
    struct Release {
        Identity origin;
        s32 storedKind = 0;
        PotionEmission::Context context;
        std::variant<PotionEmission::Burst, MissileLaunch> emission;
    };

    /** Trusted class tree is borrowed until clear/restore. Both restore and step
     * are transactional. Restoring a post-step release never re-emits that event;
     * observations contain only successful future steps after this baseline. */
    bool restore(const TreeInfo& tree, const Binding& binding, const Checkpoint& state);
    void clear();
    bool step(const Frame& frame);
    Checkpoint checkpoint() const;
    const PlayerAnimator& animator() const { return m_animator; }
    bool hasPotion() const { return !m_potions.empty(); }
    u32 remaining() const { return m_remaining; }
    std::span<const Release> releases() const { return m_releases; }

private:
    Binding m_binding;
    PlayerAnimator m_animator;
    std::vector<s32> m_potions;
    Context m_context;
    PlayerDeed m_reaction = PlayerDeed::None;
    u64 m_tick = 0;
    u32 m_remaining = 0;
    std::vector<Release> m_releases;
};
} // namespace gdl::game
