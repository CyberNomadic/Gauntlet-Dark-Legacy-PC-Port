#pragma once

#include <span>
#include <vector>

#include "engine/core/Types.h"

namespace gdl {
/** Lossless four-byte-lane permutation for compression of scalar-heavy byte
 * streams. No scalar is interpreted: signed zeros, payload bits and partial
 * trailing words are preserved independently of host byte order. */
class BytePlanes {
public:
    static std::vector<u8> encode(std::span<const u8> bytes) { return permute(bytes, false); }
    static std::vector<u8> decode(std::span<const u8> bytes) { return permute(bytes, true); }

private:
    static std::vector<u8> permute(std::span<const u8> bytes, bool reverse) {
        constexpr usize kLanes = 4;
        const usize words = bytes.size() / kLanes;
        std::vector<u8> output(bytes.size());
        for (usize lane = 0; lane < kLanes; ++lane) {
            for (usize i = 0; i < words; ++i) {
                const usize scalar = i * kLanes + lane;
                const usize planar = lane * words + i;
                output[reverse ? scalar : planar] = bytes[reverse ? planar : scalar];
            }
        }
        for (usize i = words * kLanes; i < bytes.size(); ++i) {
            output[i] = bytes[i];
        }
        return output;
    }
};
} // namespace gdl
