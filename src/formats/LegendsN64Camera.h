#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::formats {

struct LegendsN64CameraMarker {
    Vec3 position{0};
    f32 yaw = 0;
    f32 pitch = 0;
};

struct LegendsN64CameraData {
    // IST entry zero is the opening camera, not a nearest-marker candidate.
    std::vector<LegendsN64CameraMarker> markers;
    std::vector<Vec3> towerExits;
    bool tower = false;
};

/** Reads the native IST camera table. Tower's radial camera additionally needs
 * its native realm-exit directions from the extracted US game.bin. */
LegendsN64CameraData decodeLegendsN64Camera(std::span<const u8> settings,
                                            std::string_view levelName,
                                            std::span<const u8> program = {});

} // namespace gdl::formats
