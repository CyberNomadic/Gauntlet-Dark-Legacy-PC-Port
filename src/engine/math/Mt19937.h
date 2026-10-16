#pragma once

#include <array>
#include <limits>

#include "engine/core/Types.h"

namespace gdl {

/** MT19937 with a library-independent value checkpoint, not a serialized STL engine. */
class Mt19937 {
public:
    bool operator==(const Mt19937&) const = default;

    using result_type = u32;
    static constexpr usize state_size = 624;
    static constexpr u32 default_seed = 5489U;

    struct Checkpoint {
        std::array<u32, state_size> words{};
        // Index of the next untempered word; state_size means twist before the next draw.
        u32 cursor = static_cast<u32>(state_size);

        constexpr bool valid() const noexcept {
            if (cursor > state_size) {
                return false;
            }
            // The low 31 bits of word zero are discarded by the next twist. They alone
            // cannot keep an otherwise zero state out of MT19937's absorbing zero state.
            if ((words[0] & 0x80000000U) != 0) {
                return true;
            }
            for (usize index = 1; index < state_size; ++index) {
                if (words[index] != 0) {
                    return true;
                }
            }
            return false;
        }

        bool operator==(const Checkpoint&) const = default;
    };

    explicit constexpr Mt19937(u32 value = default_seed) noexcept { seed(value); }

    constexpr void seed(u32 value = default_seed) noexcept {
        m_state.words[0] = value;
        for (usize index = 1; index < state_size; ++index) {
            const u32 previous = m_state.words[index - 1];
            m_state.words[index] =
                1812433253U * (previous ^ (previous >> 30)) + static_cast<u32>(index);
        }
        m_state.cursor = static_cast<u32>(state_size);
    }

    constexpr u32 operator()() noexcept {
        if (m_state.cursor == state_size) {
            twist();
        }
        u32 value = m_state.words[m_state.cursor++];
        value ^= value >> 11;
        value ^= (value << 7) & 0x9d2c5680U;
        value ^= (value << 15) & 0xefc60000U;
        value ^= value >> 18;
        return value;
    }

    static constexpr u32 min() noexcept { return 0; }
    static constexpr u32 max() noexcept { return 0xffffffffU; }

    constexpr Checkpoint checkpoint() const noexcept { return m_state; }

    constexpr bool restore(const Checkpoint& state) noexcept {
        if (!state.valid()) {
            return false;
        }
        m_state = state;
        return true;
    }

    /**
     * Explicit binary32 mapping to [0, 1), consuming the high 24 bits of one word.
     * This matches MSVC 14.44's generate_canonical<float, 24> for MT19937, not every
     * historical std::uniform_real_distribution implementation or the GameCube RNG.
     * Keeping it separate from raw draws preserves each caller's draw count and lets
     * callers choose their interval arithmetic without a library-specific distribution.
     */
    static constexpr f32 float24(u32 word) noexcept {
        static_assert(std::numeric_limits<f32>::is_iec559 &&
                      std::numeric_limits<f32>::digits == 24);
        return static_cast<f32>(word >> 8) * 0x1p-24f;
    }

    constexpr f32 unitFloat24() noexcept { return float24((*this)()); }

private:
    constexpr void twist() noexcept {
        for (usize index = 0; index < state_size; ++index) {
            const u32 joined = (m_state.words[index] & 0x80000000U) |
                               (m_state.words[(index + 1) % state_size] & 0x7fffffffU);
            // Wrapped indices deliberately read the already updated early words,
            // exactly as the standard MT19937 recurrence does within a state block.
            m_state.words[index] = m_state.words[(index + 397) % state_size] ^ (joined >> 1) ^
                                   ((joined & 1U) != 0 ? 0x9908b0dfU : 0U);
        }
        m_state.cursor = 0;
    }

    Checkpoint m_state;
};

} // namespace gdl
