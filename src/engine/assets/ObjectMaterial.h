#pragma once

#include "engine/core/Types.h"

namespace gdl {
/** Shared interpretation of object-layer rendering flags, separate from world,
 * particle and bitmap flags. Other bits remain with their original records. */
/**
 * How an object's authored flags draw it, decoded the one way the original does
 * (MBSetupObject, AddSortObject, setPrimAlpha). `sorted` is only eligibility for the
 * deferred queue: an object joins it when it is also faded, a glow, flagged behind, or
 * its mesh is alpha-textured. Everything else draws at once, in order, writing depth.
 */
struct ObjectMaterial {
    bool additive = false;
    bool depthWrite = true;
    bool depthTest = true;
    bool chrome = false;
    bool sorted = false;       ///< may be deferred (flag 0x800)
    bool deferAlways = false; ///< a glow or sort-behind object: deferred while sorted
    u32 facing = 0;
    f32 sortBias = 0;

    static constexpr ObjectMaterial fromFlags(u32 flags) {
        f32 bias = 0.0f;
        if ((flags & 0x100400U) != 0) {
            bias = -30000.0f;
        } else if ((flags & 0x400000U) != 0) {
            bias = -20000.0f;
        } else if ((flags & 0x80000U) != 0) {
            bias = -10000.0f;
        }
        return {.additive = (flags & 0x800000U) != 0,
                .depthWrite = (flags & 0x80U) == 0,
                .depthTest = (flags & 0x40U) == 0,
                .chrome = (flags & 0x8000U) != 0,
                .sorted = (flags & 0x800U) != 0,
                .deferAlways = (flags & 0x40800000U) != 0,
                .facing = (flags >> 24U) & 0xFU,
                .sortBias = bias};
    }

    /** Whether the object is drawn after the solids, farthest first, at `alpha`. */
    constexpr bool deferred(bool meshAlphaTextured, f32 alpha = 1.0f) const {
        return sorted && (alpha < 1.0f || deferAlways || meshAlphaTextured);
    }
};
} // namespace gdl
