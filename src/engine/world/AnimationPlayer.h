#pragma once

#include "engine/assets/AnimationSet.h"
#include "engine/core/Types.h"

namespace gdl {
class NativeStateDigest;

/**
 * Plays one sequence of a tree on the game clock the way the original engine steps its
 * animations: a sequence's rate is 900 divided by the frames it shows per second, the frame
 * snaps to whole numbers, a repeating sequence retains its elapsed remainder when it wraps
 * and a one-shot holds its last frame. A transition holds the first frame while the caller
 * blends the pose across from whatever showed before.
 */
class AnimationPlayer {
public:
    /** Exact native clock comparison within one loaded asset binding. Not a
     * cross-process identity or wire checkpoint comparison. */
    bool equalLocal(const AnimationPlayer& other) const { return *this == other; }
    bool operator==(const AnimationPlayer&) const = default;
    static constexpr f32 kRateUnit = 1.0f / 900.0f; ///< seconds per frame per unit of rate
    static constexpr f32 kDefaultRate = 30.0f;
    static constexpr f32 kSnapWindow = 0.125f; ///< nearer a whole frame than this snaps to it
    static constexpr f32 kTick = 1.0f / 30.0f; ///< frames shorter than a tick always snap
    // Beyond a month without a clock reset, prediction must take a fresh seed
    // instead of admitting near-overflow clocks from an untrusted packet.
    static constexpr f64 kCheckpointSeconds = 30.0 * 24.0 * 60.0 * 60.0;
    static constexpr f32 kCheckpointFrames = 1'000'000;
    static constexpr f32 kCheckpointMinPeriod = 0.000001f;
    static constexpr f32 kCheckpointMaxPeriod = 3600;
    static constexpr f32 kCheckpointMaxSpeed = 1024;
    static constexpr u64 kCheckpointGenerationHeadroom = 1'000'000;

    /** Resumable clock state, without a borrowed asset pointer. The elapsed clock,
     * held-end flag and transition credit are necessary to reproduce boundaries;
     * restoring just the displayed frame can release an attack on a different tick. */
    struct Checkpoint {
        u32 sequence = 0;
        u64 generation = 0;
        f32 secondsPerFrame = kDefaultRate * kRateUnit;
        f64 time = 0;
        f32 frame = 0;
        f32 speed = 1;
        f32 transitionLength = 0;
        f32 transitionTime = 0;
        bool playing = false;
        bool finished = false;
        bool held = false;
        bool smooth = false;
        bool valid() const;
        bool operator==(const Checkpoint&) const = default;
    };
    Checkpoint checkpoint() const;
    /** The caller resolves the sequence index against its trusted asset table.
     * A stopped checkpoint requires nullptr. Invalid input leaves the player intact. */
    bool restore(const Checkpoint& state, const TreeSequenceInfo* sequence);

    /** Starts `sequence` at `frame`, holding it for `transitionSeconds` first.
     * An elapsed blend credit changes only the blend, never sequence playback. */
    void start(const TreeSequenceInfo& sequence, u32 index, f32 transitionSeconds = 0.0f,
               f32 frame = 0.0f, f32 transitionElapsedSeconds = 0.0f);
    void stop();

    /** Steps by `seconds`; true the step the sequence wraps or reaches its end. A finished
     * one-shot stays put until started again. */
    bool advance(f32 seconds, bool repeat);

    bool playing() const { return m_sequence != nullptr; }
    u32 sequence() const { return m_index; }
    f32 frame() const { return m_frame; }
    /** Fractional visual sample only; never use for gameplay events or collision poses. */
    f32 presentationFrame() const;
    /** Changes on a start, stop or loop wrap, where render interpolation must not cross. */
    u64 generation() const { return m_generation; }
    s32 frameCount() const { return m_sequence != nullptr ? m_sequence->frames : 0; }
    /** True from the step the sequence wrapped or ended until the next step moves on. */
    bool finished() const { return m_finished; }
    bool transitioning() const { return m_transitionTime < m_transitionLength; }
    /** How far the transition into this sequence has come, 1 once it is over. */
    f32 transition() const;

    /** The frames a repeating sequence cycles through: `count` of them from `first`, the
     * rest never shown; a frame outside them moves to the first. A new start plays the
     * whole sequence again. */
    void setCycle(s32 first, s32 count);
    s32 cycleFirst() const { return m_cycleFirst; }
    /** How many frames the cycle shows: the sequence's own when none was set. */
    s32 cycleCount() const;
    /** Lets frames fall between whole numbers when a sequence is slower than the clock. */
    void setSmooth(bool smooth) { m_smooth = smooth; }
    /** Scales playback: 2 plays twice as fast. */
    void setSpeed(f32 speed) { m_speed = speed; }
    f32 secondsPerFrame() const { return m_secondsPerFrame; }

private:
    friend void visitNativeState(NativeStateDigest& /*digest*/, const AnimationPlayer& /*state*/);
    const TreeSequenceInfo* m_sequence = nullptr;
    u32 m_index = 0;
    u64 m_generation = 0;
    f32 m_secondsPerFrame = kDefaultRate * kRateUnit;
    f64 m_time = 0.0; ///< accumulated seconds; preserve loop boundaries across small steps
    f32 m_frame = 0.0f;
    f32 m_speed = 1.0f;
    f32 m_transitionLength = 0.0f;
    f32 m_transitionTime = 0.0f;
    bool m_finished = false;
    bool m_held = false; ///< a one-shot that has ended
    bool m_smooth = false;
    s32 m_cycleFirst = 0;
    s32 m_cycleCount = 0; ///< 0: the whole sequence
};

} // namespace gdl
