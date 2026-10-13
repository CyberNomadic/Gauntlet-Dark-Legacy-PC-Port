#pragma once

#include <optional>
#include <vector>

#include "game/players/PlayerAnimator.h"

namespace gdl::game {
/** Immutable acquisition geometry at fixed feet. World/body visibility and
 * vertical eligibility are checked by the producer; facing and held reach are
 * evaluated here without advancing entities, effects, or damage. */
class MeleeSelection {
public:
    static constexpr f32 kSwingReach = 1;
    static constexpr f32 kStepReach = 2;
    static constexpr f32 kHeldReach = 1;
    static constexpr f32 kHeldCone = 0.5f;
    struct Target {
        Vec3 base{0};
        Vec3 offset{0};   ///< from the native query origin to its acquisition point
        f32 distance = 0; ///< physical surface distance, not the weighted score
        f32 score = 0;    ///< acquisition distance, including authored weighting
        bool low = false; ///< family/height qualifies; near-enough test remains dynamic
    };
    struct Contact {
        Target target;              ///< distance/offset use the native 3D swarm contact query
        bool requireFacing = false; ///< Death still requires facing when overlapping
    };
    struct Snapshot {
        Vec3 feet{0};
        f32 reach = 0;
        f32 range = 30;
        f32 facingDot = 0.707f;
        std::vector<Target> targets; ///< original acquisition order, including tie order
        std::optional<Contact> contact;
    };
    struct Selection {
        bool contact = false;
        usize index = 0;
        f32 distance = 0;
    };
    static bool accepts(const Target& target, const Vec3& facing, f32 range, f32 facingDot);
    static bool accepts(const Contact& contact, const Vec3& facing, f32 range);
    /** An accepted contact owns acquisition even beyond queryReach. Its caller
     * must reject the out-of-reach result without substituting another target. */
    static std::optional<Selection> select(const Snapshot& snapshot, const Vec3& facing,
                                           f32 queryReach);
    static MeleeSense senseOf(const Target& target, const Vec3& feet, f32 reach, f32 yaw,
                              bool held);
    /** facing is the pre-motion attack heading; yaw is the current body heading,
     * which may already have turned toward the separately assisted aim point. */
    static MeleeSense sense(const Snapshot& snapshot, bool held, const Vec3& facing, f32 yaw);
    static PlayerDeed deed(const MeleeSense& sense, bool strong, bool moved = false, s32 chain = 0);
};
} // namespace gdl::game
