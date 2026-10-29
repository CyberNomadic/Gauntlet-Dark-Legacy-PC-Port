#include "game/scenes/LegendsWorldPresentation.h"

namespace gdl::game {

LegendsWorldPresentation LegendsWorldPresentation::n64(u32 flags) {
    LegendsWorldPresentation result;
    // e0027bf0 skips hidden nodes. e001e794 checks sorted eligibility rather
    // than treating every alpha texture as a deferred object. e001ea34 selects
    // its translucent RDP render mode for 0x10000000, and independently clears
    // Z_UPD/Z_CMP for 0x80/0x40; 0x1000 bypasses RSP normal lighting.
    result.hidden = (flags & 2U) != 0;
    result.sortable = (flags & 0x800U) != 0;
    result.forceAlpha = (flags & 0x10000000U) != 0;
    result.depthWrite = (flags & 0x80U) == 0;
    result.depthTest = (flags & 0x40U) == 0;
    result.unlit = (flags & 0x1000U) != 0;
    result.facing = (flags >> 24U) & 7U;
    return result;
}

Mat4 LegendsWorldPresentation::placement(const Mat4& authored, const WorldCamera& camera) const {
    if (facing != 1) {
        return authored;
    }
    return CameraFrame::of(camera).face(authored, CameraFrame::kFacingProjectedYaw);
}

} // namespace gdl::game
