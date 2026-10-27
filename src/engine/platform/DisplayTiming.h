#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "engine/core/Types.h"

namespace gdl {

struct DisplayBounds {
    s32 x = 0;
    s32 y = 0;
    u32 width = 0;
    u32 height = 0;
};

/** Selects the display covering most of a window; equal overlap keeps the first display. */
class DisplayRefresh {
public:
    static constexpr u32 kFallbackRate = 60;

    explicit DisplayRefresh(DisplayBounds window) : m_window(window) {}
    void consider(DisplayBounds display, u32 refreshRate);
    u32 rate() const { return m_rate; }

private:
    DisplayBounds m_window;
    u64 m_largestOverlap = 0;
    u32 m_rate = kFallbackRate;
    bool m_found = false;
};

/** A zero request follows the monitor; a positive cap never exceeds its refresh rate. */
u32 displayFrameRate(u32 requested, u32 monitorRefreshRate);

/** Absolute render deadlines retain small sleep overshoots instead of accumulating them.
 * Corrections never shorten a frame by more than one quarter of its period; slow frames
 * and changed caps reset the schedule rather than producing catch-up bursts. */
class FramePacer {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;

    Time deadline(Time frameStart, Time frameFinished, u32 rate);

private:
    Time m_deadline;
    u32 m_rate = 0;
};

enum class FrameTimingPhase : u8 {
    Poll,
    Update,
    Acquire,
    Render,
    Present,
    Refresh,
    Wait,
    Oversleep,
    Count
};

/** Opt-in diagnostic measurements, never used to advance simulation or tune pacing. */
struct FrameTimingSample {
    std::array<f64, static_cast<usize>(FrameTimingPhase::Count)> milliseconds{};
    f64 elapsedMilliseconds = 0;
    u32 updates = 0;
    bool overran = false;

    void measure(FrameTimingPhase phase, FramePacer::Time begin, FramePacer::Time end);
};

struct FrameTimingTotals {
    std::array<f64, static_cast<usize>(FrameTimingPhase::Count)> milliseconds{};
    std::array<f64, static_cast<usize>(FrameTimingPhase::Count)> maximumMilliseconds{};
    f64 elapsedMilliseconds = 0;
    u32 frames = 0;
    u32 updates = 0;
    u32 overruns = 0;

    void add(const FrameTimingSample& sample);
    f64 framesPerSecond() const;
    f64 average(FrameTimingPhase phase) const;
    f64 maximum(FrameTimingPhase phase) const;
};

/** The spike tracer's setting: how long a frame's work may run before it is reported. */
struct SpikeThreshold {
    static constexpr f64 kBudgets = 3.0; ///< the automatic setting, in frame budgets
    std::optional<f64> milliseconds;      ///< none follows the display; 0 reports nothing
    bool operator==(const SpikeThreshold&) const = default;

    /** The threshold at `frameRate`, in milliseconds; 0 reports nothing. */
    f64 at(u32 frameRate) const {
        if (milliseconds.has_value()) {
            return std::max(*milliseconds, 0.0);
        }
        return kBudgets * 1000.0 / static_cast<f64>(std::max(frameRate, 1U));
    }
    /** "off", "auto" or a number of milliseconds; nothing for anything else. */
    static std::optional<SpikeThreshold> parse(std::string_view text) {
        if (text == "off" || text == "0") {
            return SpikeThreshold{0.0};
        }
        if (text == "auto") {
            return SpikeThreshold{};
        }
        f64 value = 0;
        const auto* end = text.data() + text.size();
        const auto [parsed, error] = std::from_chars(text.data(), end, value);
        if (error != std::errc{} || parsed != end || !(value > 0)) {
            return std::nullopt;
        }
        return SpikeThreshold{value};
    }
    std::string describe() const {
        if (!milliseconds.has_value()) {
            return "auto (three frame budgets)";
        }
        return *milliseconds > 0 ? std::to_string(*milliseconds) + " ms" : "off";
    }
};

} // namespace gdl
