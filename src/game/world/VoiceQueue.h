#pragma once

#include <memory>
#include <span>
#include <vector>

#include "engine/assets/SoundSet.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

#include "game/world/SceneSoundEvents.h"
#include "game/world/SpeechTiming.h"

namespace gdl::game {

/**
 * One of the original's two sound queues (sndFxQueAddEx's modes: the narrator's lines, and
 * the characters' cries): lines play one after another, at most sixteen waiting. A line asks
 * for room first, giving how long it is willing to wait behind what is already queued, and
 * is turned away whole when that is longer. The output is borrowed and may be null.
 */
class VoiceQueue {
public:
    VoiceQueue() = default;
    VoiceQueue(const VoiceQueue&) = delete;
    VoiceQueue& operator=(const VoiceQueue&) = delete;
    VoiceQueue(VoiceQueue&&) = delete;
    VoiceQueue& operator=(VoiceQueue&&) = delete;
    ~VoiceQueue() = default;
    static constexpr usize kMost = 16;
    static constexpr f32 kAlwaysRoom = -1.0f; ///< a wait that is never refused

    void bind(SoundPlayer* output);
    /** Attach before recording native history, with no queued speech. Changing
     * epochs forgets output predecessors without changing admission clocks or
     * holds. Null retains direct offline output. Rebind after reopening a recorder. */
    void recordTo(SceneSoundEvents* events);
    /** Whether a line willing to wait `maxWait` seconds may be queued now. */
    bool room(f32 maxWait) const;
    /** Queues a sequence behind the rest at `volume`; kNoSound when inaudible,
     * held or full. Logical admission does not depend on playback succeeding. */
    SoundHandle queue(const SoundSequence& sequence, f32 volume, f32 pan = 0);
    SoundCueToken logicalTail() const { return m_sim.tail; }
    /** While held nothing more is let in: the good wizard has the floor. */
    void hold(bool held) { m_sim.held = held; }
    bool held() const { return m_sim.held; }
    /** Advances the clock, letting finished lines go. */
    void update(f32 seconds);
    /** Seconds of queued lines still to come. */
    f64 backlog() const;
    /** Pointer-free logical state, including silently admitted lines. */
    std::span<const SpeechTiming> timing() const { return m_sim.lines; }
    /** Forgets what was queued without stopping it. */
    void clear();
    /** How long a sequence plays, in seconds. */
    static f64 lengthOf(const SoundSequence& sequence);

private:
    SoundPlayer* m_output = nullptr;
    SceneSoundEvents* m_events = nullptr;
    SoundHandle m_tail = kNoSound;
    struct Simulation {
        std::vector<SpeechTiming> lines;
        bool held = false;
        SoundCueToken tail;
        template <class Visitor> friend void visitNativeState(Visitor& visitor, const Simulation& state) {
            visitor.template fields<3>(state);
        }
        bool operator==(const Simulation&) const = default;
    };

public:
    /** Admission and logical predecessor are native state; mixer handles are not. */
    class LocalSnapshot {
        friend class VoiceQueue;
        Simulation m_values;
        std::weak_ptr<const u8> m_owner;
        u64 m_epoch = 0;
        u64 m_soundEpoch = 0;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const;
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;

private:
    Simulation m_sim;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
    u64 m_localEpoch = 1;
};

} // namespace gdl::game
