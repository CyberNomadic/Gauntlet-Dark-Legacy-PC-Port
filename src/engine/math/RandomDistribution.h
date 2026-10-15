#pragma once

#include <bit>
#include <optional>

#include "engine/core/Types.h"

namespace gdl::random_distribution {

/** Library-independent uniform index in [0, count), for a generator whose
 * declared closed range fits u32. Invalid counts (zero or above 2^32) refuse
 * without drawing. Even count==1 consumes a sample, preserving stream position.
 *
 * Accept the largest power-of-two subrange of raw-min, concatenate accepted
 * samples (low 32 bits when necessary), then use multiply/reject reduction.
 * This defines MSVC 14.44's mapping for our MT19937 and minstd_rand callers,
 * including rejected draws, independently of STL distributions and whether
 * an implementation's uint_fast32_t occupies 32 or 64 bits. */
template <class Generator> std::optional<u32> boundedIndex(Generator& random, u64 count) {
    static_assert(Generator::min() < Generator::max());
    static_assert(static_cast<u64>(Generator::max()) <= 0xffffffffULL);
    static_assert(static_cast<u64>(Generator::min()) <= 0xffffffffULL);
    constexpr u64 fullRange = u64{1} << 32;
    if (count == 0 || count > fullRange) {
        return std::nullopt;
    }
    constexpr u32 sourceMask = [] {
        u32 mask = 0xffffffffU;
        constexpr u64 range =
            static_cast<u64>(Generator::max()) - static_cast<u64>(Generator::min());
        while (range < mask) {
            mask >>= 1;
        }
        return mask;
    }();
    constexpr auto sourceBits = std::bit_width(sourceMask);
    const auto sample = [&] {
        u32 value;
        do {
            value = static_cast<u32>(random() - Generator::min());
        } while (value > sourceMask);
        return value;
    };
    u32 mask = sourceMask;
    u32 samples = 1;
    if constexpr (sourceBits < 32) {
        while (static_cast<u64>(mask) + 1 < count) {
            mask = (mask << sourceBits) | sourceMask;
            ++samples;
        }
    }
    const auto bits = std::bit_width(mask);
    const u64 threshold = (static_cast<u64>(mask) + 1 - count) % count;
    for (;;) {
        u32 value = sample();
        if constexpr (sourceBits < 32) {
            for (u32 index = 1; index < samples; ++index) {
                value = (value << sourceBits) | sample();
            }
        }
        const u64 product = static_cast<u64>(value) * count;
        if ((product & mask) >= threshold) {
            return static_cast<u32>(product >> bits);
        }
    }
}

} // namespace gdl::random_distribution
