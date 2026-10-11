#include "game/netplay/FeedbackSync.h"

#include <algorithm>
#include <limits>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDFB");
enum class Kind : u8 { Pulse, Gate };

void word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
void wide(std::vector<u8>& bytes, u64 value) {
    word(bytes, static_cast<u32>(value));
    word(bytes, static_cast<u32>(value >> 32));
}
u64 wide(ByteReader& reader) {
    const u64 low = reader.readU32();
    return low | (static_cast<u64>(reader.readU32()) << 32);
}
std::vector<u8> packet(u64 epoch, Kind kind, u8 seat, u8 value, u8 frames, u32 grant,
                       u64 generation, u64 sequence) {
    std::vector<u8> bytes;
    bytes.reserve(FeedbackSync::kPacketBytes);
    word(bytes, kMagic);
    bytes.insert(bytes.end(), {1, static_cast<u8>(kind), 0, 0});
    wide(bytes, epoch);
    bytes.insert(bytes.end(), {seat, value, frames, 0});
    word(bytes, grant);
    wide(bytes, generation);
    wide(bytes, sequence);
    return bytes;
}
bool validSeat(s32 seat) {
    return seat >= 0 && seat < 4;
}
bool validFeedback(u8 feedback) {
    return feedback <= static_cast<u8>(ControlFeedback::ThunderHammer);
}
} // namespace

bool FeedbackSync::recognizes(std::span<const u8> bytes) {
    return bytes.size() >= 4 && ByteReader(bytes).readU32() == kMagic;
}
void FeedbackSync::begin(u64 epoch, u8 localPeer, const std::array<u8, 4>& owners,
                         const std::array<u32, 4>& grants) {
    clear();
    if (epoch == 0 || localPeer == 0 || localPeer > 4 ||
        std::ranges::find(owners, localPeer) == owners.end() ||
        std::ranges::find(owners, u8{1}) == owners.end()) {
        return;
    }
    for (usize seat = 0; seat < owners.size(); ++seat) {
        if (owners[seat] > 4 || ((owners[seat] == 0) != (grants[seat] == 0))) {
            return;
        }
    }
    m_epoch = epoch;
    m_local = localPeer;
    m_owners = owners;
    m_grants = grants;
    m_generations.fill(1);
    m_enabled.fill(true);
}
void FeedbackSync::clear() {
    m_epoch = 0;
    m_local = 0;
    m_owners = {};
    m_grants = {};
    m_generations = {};
    m_sequences = {};
    m_enabled = {};
    m_pending.clear();
    m_outgoing.clear();
}
void FeedbackSync::discardSeat(u8 seat) {
    std::erase_if(m_pending, [seat](const auto& event) { return event.seat == seat; });
    // Every queued packet was built here and has the same fixed seat field.
    std::erase_if(m_outgoing, [seat](const auto& delivery) { return delivery.bytes[16] == seat; });
}
void FeedbackSync::retain(const FeedbackEvent& event) {
    if (m_pending.size() == kBudget) {
        m_pending.pop_front();
    }
    m_pending.push_back(event);
}
bool FeedbackSync::emit(s32 seat, s32 frames, ControlFeedback feedback) {
    if (m_epoch == 0 || m_local != 1 || !validSeat(seat) || frames < 0 || frames > 30 ||
        !validFeedback(static_cast<u8>(feedback))) {
        return false;
    }
    const auto index = static_cast<usize>(seat);
    const auto owner = m_owners[index];
    if (owner == 0) {
        return false;
    }
    if (!m_enabled[index]) {
        return true;
    }
    if (m_sequences[index] == std::numeric_limits<u64>::max()) {
        return false;
    }
    const auto sequence = ++m_sequences[index];
    if (owner == m_local) {
        retain({static_cast<u8>(seat), frames, feedback});
    } else {
        if (m_outgoing.size() == kBudget) {
            m_outgoing.pop_front();
        }
        m_outgoing.push_back({owner, packet(m_epoch, Kind::Pulse, static_cast<u8>(seat),
                                            static_cast<u8>(feedback), static_cast<u8>(frames),
                                            m_grants[index], m_generations[index], sequence)});
    }
    return true;
}
bool FeedbackSync::setEnabled(s32 seat, bool enabled) {
    if (m_epoch == 0 || !validSeat(seat)) {
        return false;
    }
    const auto index = static_cast<usize>(seat);
    if (m_owners[index] != m_local) {
        return false;
    }
    if (m_enabled[index] == enabled) {
        return true;
    }
    if (m_generations[index] == std::numeric_limits<u64>::max()) {
        return false;
    }
    ++m_generations[index];
    m_enabled[index] = enabled;
    discardSeat(static_cast<u8>(seat));
    if (m_local != 1) {
        // A guest queues only gates, at most one for each of its four seats.
        m_outgoing.push_back(
            {1, packet(m_epoch, Kind::Gate, static_cast<u8>(seat), static_cast<u8>(enabled), 0,
                       m_grants[index], m_generations[index], 0)});
    }
    return true;
}
FeedbackSync::Admission FeedbackSync::receive(u8 peer, std::span<const u8> bytes) {
    if (bytes.size() != kPacketBytes || !recognizes(bytes)) {
        return Admission::Invalid;
    }
    ByteReader reader(bytes);
    reader.readU32();
    if (reader.readU8() != 1) {
        return Admission::Invalid;
    }
    const auto kind = static_cast<Kind>(reader.readU8());
    if (reader.readU16() != 0) {
        return Admission::Invalid;
    }
    const auto epoch = wide(reader);
    const auto seat = reader.readU8();
    const auto value = reader.readU8();
    const auto frames = reader.readU8();
    if (reader.readU8() != 0 || seat >= m_owners.size()) {
        return Admission::Invalid;
    }
    const auto grant = reader.readU32();
    const auto generation = wide(reader);
    const auto sequence = wide(reader);
    if (m_epoch == 0 || epoch != m_epoch) {
        return epoch < m_epoch ? Admission::Stale : Admission::Invalid;
    }
    if (peer == 0 || peer > 4 || peer == m_local ||
        std::ranges::find(m_owners, peer) == m_owners.end()) {
        return Admission::WrongPeer;
    }
    if (kind == Kind::Gate) {
        if (m_local != 1 || m_owners[seat] != peer || m_grants[seat] != grant) {
            return Admission::WrongPeer;
        }
        if (value > 1 || frames != 0 || sequence != 0 || generation == 0) {
            return Admission::Invalid;
        }
        if (generation <= m_generations[seat]) {
            return Admission::Stale;
        }
        discardSeat(seat);
        m_generations[seat] = generation;
        m_enabled[seat] = value != 0;
        return Admission::Accepted;
    }
    if (kind == Kind::Pulse) {
        if (m_local == 1 || peer != 1 || m_owners[seat] != m_local || m_grants[seat] != grant) {
            return Admission::WrongPeer;
        }
        if (!validFeedback(value) || frames > 30 || generation == 0 || sequence == 0) {
            return Admission::Invalid;
        }
        if (generation != m_generations[seat] || sequence <= m_sequences[seat] ||
            !m_enabled[seat]) {
            return Admission::Stale;
        }
        m_sequences[seat] = sequence;
        retain({seat, frames, static_cast<ControlFeedback>(value)});
        return Admission::Accepted;
    }
    return Admission::Invalid;
}
std::optional<FeedbackEvent> FeedbackSync::take() {
    if (m_pending.empty()) {
        return std::nullopt;
    }
    const auto event = m_pending.front();
    m_pending.pop_front();
    return event;
}
void FeedbackSync::sent() {
    if (!m_outgoing.empty()) {
        m_outgoing.pop_front();
    }
}

} // namespace gdl::game
