#pragma once

#include <deque>
#include <optional>
#include <span>

#include "engine/audio/SoundPlayer.h"

#include "game/netplay/ShotIdentity.h"

namespace gdl::game {
/** Ordered native sound commands, separate from replaceable visual snapshots.
 * Only the authenticated host can emit. Epoch changes discard old loops/cues;
 * no PCM, local mixer settings, device handles or filesystem paths cross the wire. */
class AudioSync {
public:
    enum class Admission : u8 { Accepted, Stale, WrongPeer, Invalid };
    struct Delivery {
        u8 peer;
        std::vector<u8> bytes;
    };
    struct Cue {
        SoundPlayer::Event event;
        std::optional<ShotIdentity> origin;
    };
    static constexpr usize kBudget = 4096;
    static constexpr usize kPacketBytes = 112;
    static bool recognizes(std::span<const u8> bytes);
    void begin(u64 epoch, u8 local, const std::array<u8, 4>& owners);
    void clear();
    bool emit(const SoundPlayer::Event& event, std::optional<ShotIdentity> origin = std::nullopt);
    Admission receive(u8 peer, std::span<const u8> bytes);
    std::optional<Cue> take();
    const std::deque<Delivery>& outgoing() const { return m_outgoing; }
    void sent();
    bool failed() const { return m_failed; }

private:
    u64 m_epoch = 0;
    u64 m_sequence = 0;
    u8 m_local = 0;
    std::array<u8, 4> m_owners{};
    std::array<bool, 5> m_peers{};
    bool m_failed = false;
    std::deque<Delivery> m_outgoing;
    std::deque<Cue> m_pending;
};
} // namespace gdl::game
