#pragma once

#include "engine/assets/ObjectFlags.h"
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
    bool sorted = false;      ///< may be deferred (flag 0x800)
    bool deferAlways = false; ///< a glow or sort-behind object: deferred while sorted
    u32 facing = 0;
    f32 sortBias = 0;
    f32 distanceFadeRange = 0; ///< view-depth range of an authored near-camera fade
    f32 distanceSortScale = 1; ///< AddDistObject's separate near-fade ordering rule

    static constexpr ObjectMaterial fromFlags(u32 flags) {
        using namespace object_flags;
        f32 bias = 0.0f;
        if ((flags & kDistanceFade) != 0) {
            bias = -30000.0f;
        } else if ((flags & kSortBehind) != 0) {
            bias = -20000.0f;
        } else if ((flags & kSortBack) != 0) {
            bias = -10000.0f;
        }
        return {.additive = (flags & kAdditive) != 0,
                .depthWrite = (flags & kNoDepthWrite) == 0,
                .depthTest = (flags & kNoDepthTest) == 0,
                .chrome = (flags & kChrome) != 0,
                .sorted = (flags & kSorted) != 0,
                .deferAlways = (flags & kDeferAlways) != 0,
                .facing = (flags >> kFacingShift) & kFacingMask,
                .sortBias = bias,
                // MBSetupObject: lbl_80343EC0 / lbl_80343EC4, 100 / 200 world units.
                .distanceFadeRange = (flags & kDistanceFade) == 0 ? 0.0f
                                     : (flags & 0x100000) != 0    ? 200.0f
                                                                  : 100.0f,
                .distanceSortScale = (flags & kSortBehind) != 0 ? 0.00001f
                                     : (flags & kSortBack) != 0 ? 0.001f
                                                                : 1.0f};
    }

    /** Whether the object is drawn after the solids, farthest first, at `alpha`. */
    constexpr bool deferred(bool meshAlphaTextured, f32 alpha = 1.0f) const {
        return sorted && (alpha < 1.0f || deferAlways || meshAlphaTextured);
    }
};
} // namespace gdl
