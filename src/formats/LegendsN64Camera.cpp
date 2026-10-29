#include "formats/LegendsN64Camera.h"

#include <array>
#include <bit>
#include <cmath>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {
namespace {
void require(bool condition, std::string_view message) {
    if (!condition) {
        throw FormatError("N64 Legends camera: " + std::string(message));
    }
}

void range(std::span<const u8> bytes, usize at, usize size) {
    require(at <= bytes.size() && size <= bytes.size() - at, "truncated native data");
}
} // namespace

LegendsN64CameraData decodeLegendsN64Camera(std::span<const u8> settings,
                                            std::string_view levelName,
                                            std::span<const u8> program) {
    range(settings, 0, 76);
    const usize offset = readU32BE(settings, 0);
    const usize count = readU16BE(settings, 8);
    require(offset >= 76 && count >= 2 && count <= 4096, "invalid IST camera table");
    range(settings, offset, count * 8);
    LegendsN64CameraData result;
    result.markers.reserve(count);
    // US e0037824/e0035018/e00370c8: signed XYZ quarters, signed angle bytes.
    // Yaw spans a full turn in 256 steps; pitch spans a half turn in 256.
    for (usize i = 0; i < count; ++i) {
        const usize at = offset + i * 8;
        LegendsN64CameraMarker marker;
        for (usize axis = 0; axis < 3; ++axis) {
            marker.position[axis] =
                static_cast<f32>(static_cast<s16>(readU16BE(settings, at + axis * 2))) / 4;
        }
        marker.yaw = static_cast<f32>(static_cast<s8>(settings[at + 6])) / 40.743664f;
        marker.pitch = static_cast<f32>(static_cast<s8>(settings[at + 7])) / 81.48733f;
        result.markers.push_back(marker);
    }
    result.tower = toLowerAscii(levelName) == "tower2";
    if (result.tower) {
        // e0869a6c projects the tower target onto the nearest entry in this
        // native realm-return table. The five directions are NOT IST markers.
        constexpr usize table = 0xe006eedcU - 0xe0000400U;
        constexpr std::array<u32, 5> realms{0, 3, 7, 6, 12};
        range(program, table, (realms.size() + 1) * 16);
        for (usize i = 0; i < realms.size(); ++i) {
            const usize at = table + i * 16;
            require(readU32BE(program, at) == realms[i], "unsupported tower return table");
            Vec3 point;
            for (usize axis = 0; axis < 3; ++axis) {
                point[axis] = std::bit_cast<f32>(readU32BE(program, at + 4 + axis * 4));
                require(std::isfinite(point[axis]) && std::abs(point[axis]) <= 100000,
                        "invalid tower return point");
            }
            require(point.x * point.x + point.z * point.z > .0001f, "zero tower return direction");
            result.towerExits.push_back(point);
        }
        require(readU32BE(program, table + realms.size() * 16) == 16,
                "unterminated tower return table");
    }
    return result;
}

} // namespace gdl::formats
