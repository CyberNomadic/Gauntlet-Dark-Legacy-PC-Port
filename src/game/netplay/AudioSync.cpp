#include "game/netplay/AudioSync.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDAS");
constexpr u32 kVersion = 3;
constexpr usize kOriginOffset = 76;
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
bool valid(const SoundPlayer::Event& event) {
    using Kind = SoundPlayer::Event::Kind;
    if (event.handle == kNoSound || event.kind > Kind::Pan ||
        event.category >= SoundCategory::Count || !std::isfinite(event.value) ||
        event.value < (event.kind == Kind::Pan ? -1.0f : 0.0f) || event.value > 1) {
        return false;
    }
    if (event.kind != Kind::Play) {
        return event.bank.empty() && event.call == 0 && event.after == kNoSound;
    }
    return !event.bank.empty() && event.bank.size() <= 32 && event.call < 65536 &&
           event.after != event.handle && std::ranges::all_of(event.bank, [](char value) {
               return (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
                      value == '_';
           });
}
bool validOrigin(const SoundPlayer::Event& event, const std::optional<ShotIdentity>& origin,
                 u64 epoch, const std::array<u8, 4>& owners) {
    return !origin || (origin->valid() && origin->epoch == epoch && origin->ordinal == 0 &&
                       owners[origin->seat] != 0 && event.kind == SoundPlayer::Event::Kind::Play &&
                       event.category == SoundCategory::Effects && event.after == kNoSound);
}
} // namespace
bool AudioSync::recognizes(std::span<const u8> bytes) {
    return bytes.size() >= 4 && ByteReader(bytes).readU32() == kMagic;
}
void AudioSync::clear() {
    m_epoch = 0;
    m_sequence = 0;
    m_local = 0;
    m_owners = {};
    m_peers = {};
    m_failed = false;
    m_outgoing.clear();
    m_pending.clear();
}
void AudioSync::begin(u64 epoch, u8 local, const std::array<u8, 4>& owners) {
    clear();
    if (epoch == 0 || local == 0 || local > 4 || std::ranges::find(owners, local) == owners.end() ||
        std::ranges::find(owners, u8{1}) == owners.end() ||
        std::ranges::any_of(owners, [](u8 owner) { return owner > 4; })) {
        return;
    }
    m_epoch = epoch;
    m_local = local;
    m_owners = owners;
    for (const auto owner : owners) {
        m_peers[owner] = owner != 0;
    }
}
bool AudioSync::emit(const SoundPlayer::Event& event, std::optional<ShotIdentity> origin) {
    if (m_epoch == 0 || m_local != 1 || m_failed) {
        return false;
    }
    if (!valid(event) || !validOrigin(event, origin, m_epoch, m_owners)) {
        return false;
    }
    const auto recipients = static_cast<usize>(std::ranges::count(m_peers, true)) - 1;
    if (m_outgoing.size() + recipients > kBudget || m_sequence == std::numeric_limits<u64>::max()) {
        // Do not silently discard a loop's stop command under congestion.
        m_failed = true;
        return false;
    }
    std::vector<u8> bytes;
    bytes.reserve(kPacketBytes);
    word(bytes, kMagic);
    word(bytes, kVersion);
    wide(bytes, m_epoch);
    wide(bytes, ++m_sequence);
    bytes.insert(bytes.end(),
                 {static_cast<u8>(event.kind), static_cast<u8>(event.category),
                  static_cast<u8>(event.bank.size()), static_cast<u8>(origin.has_value())});
    word(bytes, event.handle);
    word(bytes, event.after);
    word(bytes, event.call);
    word(bytes, std::bit_cast<u32>(event.value));
    bytes.insert(bytes.end(), event.bank.begin(), event.bank.end());
    bytes.resize(kOriginOffset, 0);
    if (origin) {
        wide(bytes, origin->epoch);
        wide(bytes, origin->tick);
        wide(bytes, origin->generation);
        word(bytes, origin->grant);
        bytes.insert(bytes.end(),
                     {origin->seat, static_cast<u8>(origin->strong), origin->ordinal, 0});
        word(bytes, origin->emission);
    }
    bytes.resize(kPacketBytes, 0);
    for (u8 peer = 2; peer <= 4; ++peer) {
        if (m_peers[peer]) {
            m_outgoing.push_back({peer, bytes});
        }
    }
    return true;
}
AudioSync::Admission AudioSync::receive(u8 peer, std::span<const u8> bytes) {
    if (peer != 1 || m_local <= 1) {
        return Admission::WrongPeer;
    }
    if (bytes.size() != kPacketBytes || !recognizes(bytes)) {
        return Admission::Invalid;
    }
    ByteReader reader(bytes);
    reader.readU32();
    if (reader.readU32() != kVersion) {
        return Admission::Invalid;
    }
    const auto epoch = wide(reader);
    const auto sequence = wide(reader);
    if (epoch != m_epoch || m_epoch == 0) {
        return epoch < m_epoch ? Admission::Stale : Admission::Invalid;
    }
    SoundPlayer::Event event;
    event.kind = static_cast<SoundPlayer::Event::Kind>(reader.readU8());
    event.category = static_cast<SoundCategory>(reader.readU8());
    const auto length = reader.readU8();
    const auto flags = reader.readU8();
    if (length > 32 || flags > 1) {
        return Admission::Invalid;
    }
    event.handle = reader.readU32();
    event.after = reader.readU32();
    event.call = reader.readU32();
    event.value = std::bit_cast<f32>(reader.readU32());
    for (u32 index = 0; index < 32; ++index) {
        const auto value = reader.readU8();
        if (index < length) {
            event.bank.push_back(static_cast<char>(value));
        } else if (value != 0) {
            return Admission::Invalid;
        }
    }
    std::optional<ShotIdentity> origin;
    if (flags != 0) {
        ShotIdentity identity;
        identity.epoch = wide(reader);
        identity.tick = wide(reader);
        identity.generation = wide(reader);
        identity.grant = reader.readU32();
        identity.seat = reader.readU8();
        const auto strong = reader.readU8();
        identity.ordinal = reader.readU8();
        if (strong > 1 || reader.readU8() != 0) {
            return Admission::Invalid;
        }
        identity.strong = strong != 0;
        identity.emission = reader.readU32();
        origin = identity;
    } else if (std::ranges::any_of(bytes.subspan(kOriginOffset),
                                   [](u8 value) { return value != 0; })) {
        return Admission::Invalid;
    }
    if (!valid(event) || !validOrigin(event, origin, m_epoch, m_owners) || sequence == 0) {
        return Admission::Invalid;
    }
    if (sequence <= m_sequence) {
        return Admission::Stale;
    }
    if (sequence != m_sequence + 1 || m_pending.size() == kBudget) {
        return Admission::Invalid;
    }
    m_sequence = sequence;
    m_pending.push_back({std::move(event), origin});
    return Admission::Accepted;
}
std::optional<AudioSync::Cue> AudioSync::take() {
    if (m_pending.empty()) {
        return std::nullopt;
    }
    auto event = std::move(m_pending.front());
    m_pending.pop_front();
    return event;
}
void AudioSync::sent() {
    if (!m_outgoing.empty()) {
        m_outgoing.pop_front();
    }
}
} // namespace gdl::game
