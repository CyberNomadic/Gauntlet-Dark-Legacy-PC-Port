#pragma once

#include <array>
#include <string_view>

#include "engine/core/Types.h"

namespace gdl::game {

/**
 * The grades of a run of close blows in Aggressor Mode, D to S, shared by the meter that
 * shows them, the swarm that answers them and the quiet that ends the run: the higher the
 * grade, the less quiet the run survives, three seconds at D down to one at S.
 */
struct StreakGrades {
    static constexpr s32 kCount = 5;
    /** The kills that reach each grade. */
    static constexpr std::array<s32, kCount> kCounts{1, 25, 50, 75, 100};
    static constexpr std::string_view kLetters = "DCBAS";
    static constexpr s32 kTop = kCounts[kCount - 1]; ///< S
    static constexpr f32 kLongestQuiet = 3.0f;       ///< the window at D
    static constexpr f32 kShortestQuiet = 1.0f;      ///< and at S

    /** The grade a count has reached: -1 before the first kill, else into `kLetters`. */
    static constexpr s32 gradeOf(s32 count) {
        s32 grade = -1;
        for (s32 i = 0; i < kCount; ++i) {
            if (count >= kCounts[static_cast<usize>(i)]) {
                grade = i;
            }
        }
        return grade;
    }

    /** The quiet a run of `count` survives, in seconds: a step down each grade. */
    static constexpr f32 quietWindow(s32 count) {
        const s32 grade = gradeOf(count);
        if (grade <= 0) {
            return kLongestQuiet;
        }
        const f32 step = (kLongestQuiet - kShortestQuiet) / static_cast<f32>(kCount - 1);
        return kLongestQuiet - step * static_cast<f32>(grade);
    }
};

} // namespace gdl::game
