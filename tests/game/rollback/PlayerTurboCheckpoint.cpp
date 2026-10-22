#include "game/rollback/PlayerTurboCheckpoint.h"

#include <bit>
#include <utility>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
void word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
void real(std::vector<u8>& bytes, f32 value) {
    word(bytes, std::bit_cast<u32>(value));
}
f32 real(ByteReader& reader) {
    return std::bit_cast<f32>(reader.readU32());
}
void rows(std::vector<u8>& bytes, const std::vector<s32>& values) {
    for (const s32 value : values) {
        word(bytes, static_cast<u32>(value));
    }
}
std::vector<s32> rows(ByteReader& reader, u32 count) {
    std::vector<s32> values;
    values.reserve(count);
    for (u32 index = 0; index < count; ++index) {
        values.push_back(reader.readS32());
    }
    return values;
}
} // namespace

bool PlayerTurboState::valid() const {
    return TurboMeter::valid(meter) && TurboMove::valid(move);
}

std::optional<std::vector<u8>> PlayerTurboCheckpoint::encode(const PlayerTurboState& state) {
    if (!state.valid()) {
        return std::nullopt;
    }
    const auto& meter = state.meter;
    const auto& move = state.move;
    std::vector<u8> bytes;
    bytes.reserve(kHeaderBytes +
                  4 * (move.pending.size() + move.all.size() + move.volleysShot.size()));
    real(bytes, meter.held);
    real(bytes, meter.shown);
    word(bytes, static_cast<u32>(meter.flash));
    word(bytes, static_cast<u32>(meter.flashTicks));
    real(bytes, move.owed);
    word(bytes, (move.named ? 1U : 0U) | (move.weaponHidden ? 2U : 0U));
    word(bytes, static_cast<u32>(move.pending.size()));
    word(bytes, static_cast<u32>(move.all.size()));
    word(bytes, static_cast<u32>(move.volleysShot.size()));
    rows(bytes, move.pending);
    rows(bytes, move.all);
    rows(bytes, move.volleysShot);
    return bytes;
}

std::optional<PlayerTurboState> PlayerTurboCheckpoint::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    PlayerTurboState state;
    auto& meter = state.meter;
    auto& move = state.move;
    meter.held = real(reader);
    meter.shown = real(reader);
    const u32 flash = reader.readU32();
    meter.flashTicks = reader.readS32();
    move.owed = real(reader);
    const u32 flags = reader.readU32();
    const u32 pendingCount = reader.readU32();
    const u32 allCount = reader.readU32();
    const u32 volleyCount = reader.readU32();
    if (flash > static_cast<u32>(TurboMeter::Flash::Glow) || (flags & ~3U) != 0 ||
        pendingCount > TurboMove::kMaxCheckpointRows || allCount > TurboMove::kMaxCheckpointRows ||
        volleyCount > TurboMove::kMaxCheckpointRows ||
        bytes.size() !=
            kHeaderBytes + 4 * (static_cast<usize>(pendingCount) + allCount + volleyCount)) {
        return std::nullopt;
    }
    meter.flash = static_cast<TurboMeter::Flash>(flash);
    move.named = (flags & 1U) != 0;
    move.weaponHidden = (flags & 2U) != 0;
    move.pending = rows(reader, pendingCount);
    move.all = rows(reader, allCount);
    move.volleysShot = rows(reader, volleyCount);
    return state.valid() ? std::optional{std::move(state)} : std::nullopt;
}

} // namespace gdl::game
