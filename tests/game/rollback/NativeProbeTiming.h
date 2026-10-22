#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::test {

struct NativeTimingSummary {
    usize samples = 0;
    f64 mean = 0;
    f64 p50 = 0;
    f64 p95 = 0;
    f64 p99 = 0;
    f64 maximum = 0;
};

// Milliseconds, with nearest-rank percentiles. Summarize outside the measured
// update: sorting and report allocation must not count as gameplay work.
inline std::optional<NativeTimingSummary> summarizeNativeTimings(std::span<const f64> samples) {
    if (samples.empty() || std::ranges::any_of(samples, [](f64 sample) {
            return !std::isfinite(sample) || sample < 0;
        })) {
        return std::nullopt;
    }
    std::vector<f64> sorted(samples.begin(), samples.end());
    std::ranges::sort(sorted);
    f64 mean = 0;
    for (usize index = 0; index < sorted.size(); ++index) {
        mean += (sorted[index] - mean) / static_cast<f64>(index + 1);
    }
    const auto percentile = [&](f64 fraction) {
        const auto rank = static_cast<usize>(std::ceil(fraction * static_cast<f64>(sorted.size())));
        return sorted[rank - 1];
    };
    return NativeTimingSummary{sorted.size(),    mean,         percentile(0.5), percentile(0.95),
                               percentile(0.99), sorted.back()};
}

} // namespace gdl::test
